/*-------------------------------------------------------------------------
 *
 * fast_path_router_planner.c
 *
 * Planning logic for fast path router planner queries. In this context,
 * we define "Fast Path Planning" as trivial queries where Citus
 * can skip relying on the standard_planner() and handle all the planning.
 *
 * For router planner, standard_planner() is mostly important to generate
 * the necessary restriction information. Later, the restriction information
 * generated by the standard_planner is used to decide whether all the shards
 * that a distributed query touches reside on a single worker node. However,
 * standard_planner() does a lot of extra things such as cost estimation and
 * execution path generations which are completely unnecessary in the context
 * of distributed planning.
 *
 * There are certain types of queries where Citus could skip relying on
 * standard_planner() to generate the restriction information. For queries
 * in the following format, Citus does not need any information that the
 * standard_planner() generates:
 *   SELECT ... FROM single_table WHERE distribution_key = X;  or
 *   DELETE FROM single_table WHERE distribution_key = X; or
 *   UPDATE single_table SET value_1 = value_2 + 1 WHERE distribution_key = X;
 *
 * Note that the queries might not be as simple as the above such that
 * GROUP BY, WINDOW FUNCIONS, ORDER BY or HAVING etc. are all acceptable. The
 * only rule is that the query is on a single distributed (or reference) table
 * and there is a "distribution_key = X;" in the WHERE clause. With that, we
 * could use to decide the shard that a distributed query touches reside on
 * a worker node.
 *
 * Copyright (c) Citus Data, Inc.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "distributed/pg_version_constants.h"

#include "distributed/distributed_planner.h"
#include "distributed/insert_select_planner.h"
#include "distributed/multi_physical_planner.h" /* only to use some utility functions */
#include "distributed/metadata_cache.h"
#include "distributed/multi_router_planner.h"
#include "distributed/pg_dist_partition.h"
#include "distributed/shardinterval_utils.h"
#include "distributed/shard_pruning.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "optimizer/optimizer.h"
#include "tcop/pquery.h"

bool EnableFastPathRouterPlanner = true;

static bool ColumnAppearsMultipleTimes(Node *quals, Var *distributionKey);
static bool ConjunctionContainsColumnFilter(Node *node, Var *column,
											Node **distributionKeyValue);
static bool DistKeyInSimpleOpExpression(Expr *clause, Var *distColumn,
										Node **distributionKeyValue);


/*
 * FastPathPlanner is intended to be used instead of standard_planner() for trivial
 * queries defined by FastPathRouterQuery().
 *
 * The basic idea is that we need a very little of what standard_planner() does for
 * the trivial queries. So skip calling standard_planner() to save CPU cycles.
 *
 */
PlannedStmt *
FastPathPlanner(Query *originalQuery, Query *parse, ParamListInfo boundParams)
{
	/*
	 * Citus planner relies on some of the transformations on constant
	 * evaluation on the parse tree.
	 */
	parse->targetList =
		(List *) eval_const_expressions(NULL, (Node *) parse->targetList);
	parse->jointree->quals =
		(Node *) eval_const_expressions(NULL, (Node *) parse->jointree->quals);

	PlannedStmt *result = GeneratePlaceHolderPlannedStmt(originalQuery);

	return result;
}


/*
 * GeneratePlaceHolderPlannedStmt creates a planned statement which contains
 * a sequential scan on the relation that is accessed by the input query.
 * The returned PlannedStmt is not proper (e.g., set_plan_references() is
 * not called on the plan or the quals are not set), so should not be
 * passed to the executor directly. This is only useful to have a
 * placeholder PlannedStmt where target list is properly set. Note that
 * this is what router executor relies on.
 *
 * This function makes the assumption (and the assertion) that
 * the input query is in the form defined by FastPathRouterQuery().
 */
PlannedStmt *
GeneratePlaceHolderPlannedStmt(Query *parse)
{
	PlannedStmt *result = makeNode(PlannedStmt);
	SeqScan *seqScanNode = makeNode(SeqScan);
	Plan *plan = &seqScanNode->plan;

	Node *distKey PG_USED_FOR_ASSERTS_ONLY = NULL;

	AssertArg(FastPathRouterQuery(parse, &distKey));

	/* there is only a single relation rte */
	seqScanNode->scanrelid = 1;

	plan->targetlist =
		copyObject(FetchStatementTargetList((Node *) parse));

	plan->qual = NULL;
	plan->lefttree = NULL;
	plan->righttree = NULL;
	plan->plan_node_id = 1;

	/*  rtable is used for access permission checks */
	result->commandType = parse->commandType;
	result->queryId = parse->queryId;
	result->stmt_len = parse->stmt_len;

	result->rtable = copyObject(parse->rtable);
	result->planTree = (Plan *) plan;
	result->hasReturning = (parse->returningList != NIL);

	Oid relationId = ExtractFirstCitusTableId(parse);
	result->relationOids = list_make1_oid(relationId);

	return result;
}


/*
 * FastPathRouterQuery gets a query and returns true if the query is eligable for
 * being a fast path router query.
 * The requirements for the fast path query can be listed below:
 *
 *   - SELECT query without CTES, sublinks-subqueries, set operations
 *   - The query should touch only a single hash distributed or reference table
 *   - The distribution with equality operator should be in the WHERE clause
 *      and it should be ANDed with any other filters. Also, the distribution
 *      key should only exists once in the WHERE clause. So basically,
 *          SELECT ... FROM dist_table WHERE dist_key = X
 *      If the filter is a const, distributionKeyValue is set
 *   - All INSERT statements (including multi-row INSERTs) as long as the commands
 *     don't have any sublinks/CTEs etc
 */
bool
FastPathRouterQuery(Query *query, Node **distributionKeyValue)
{
	FromExpr *joinTree = query->jointree;
	Node *quals = NULL;

	if (!EnableFastPathRouterPlanner)
	{
		return false;
	}

	/*
	 * We want to deal with only very simple queries. Some of the
	 * checks might be too restrictive, still we prefer this way.
	 */
	if (query->cteList != NIL || query->hasSubLinks ||
		query->setOperations != NULL || query->hasTargetSRFs ||
		query->hasModifyingCTE)
	{
		return false;
	}

	if (CheckInsertSelectQuery(query))
	{
		/* we don't support INSERT..SELECT in the fast-path */
		return false;
	}
	else if (query->commandType == CMD_INSERT)
	{
		/* we don't need to do any further checks, all INSERTs are fast-path */
		return true;
	}

	/* make sure that the only range table in FROM clause */
	if (list_length(query->rtable) != 1)
	{
		return false;
	}

	RangeTblEntry *rangeTableEntry = (RangeTblEntry *) linitial(query->rtable);
	if (rangeTableEntry->rtekind != RTE_RELATION)
	{
		return false;
	}

	/* we don't want to deal with append/range distributed tables */
	Oid distributedTableId = rangeTableEntry->relid;
	CitusTableCacheEntry *cacheEntry = GetCitusTableCacheEntry(distributedTableId);
	if (IsCitusTableTypeCacheEntry(cacheEntry, RANGE_DISTRIBUTED) ||
		IsCitusTableTypeCacheEntry(cacheEntry, APPEND_DISTRIBUTED))
	{
		return false;
	}

	/* WHERE clause should not be empty for distributed tables */
	if (joinTree == NULL ||
		(IsCitusTableTypeCacheEntry(cacheEntry, DISTRIBUTED_TABLE) && joinTree->quals ==
		 NULL))
	{
		return false;
	}

	/* if that's a reference table, we don't need to check anything further */
	Var *distributionKey = PartitionColumn(distributedTableId, 1);
	if (!distributionKey)
	{
		return true;
	}

	/* convert list of expressions into expression tree for further processing */
	quals = joinTree->quals;
	if (quals != NULL && IsA(quals, List))
	{
		quals = (Node *) make_ands_explicit((List *) quals);
	}

	/*
	 * Distribution column must be used in a simple equality match check and it must be
	 * place at top level conjustion operator. In simple words, we should have
	 *	    WHERE dist_key = VALUE [AND  ....];
	 *
	 *	We're also not allowing any other appearances of the distribution key in the quals.
	 *
	 *	Overall the logic is might sound fuzzy since it involves two individual checks:
	 *	    (a) Check for top level AND operator with one side being "dist_key = const"
	 *	    (b) Only allow single appearance of "dist_key" in the quals
	 *
	 *	This is to simplify both of the individual checks and omit various edge cases
	 *	that might arise with multiple distribution keys in the quals.
	 */
	if (ConjunctionContainsColumnFilter(quals, distributionKey, distributionKeyValue) &&
		!ColumnAppearsMultipleTimes(quals, distributionKey))
	{
		return true;
	}

	return false;
}


/*
 * ColumnAppearsMultipleTimes returns true if the given input
 * appears more than once in the quals.
 */
static bool
ColumnAppearsMultipleTimes(Node *quals, Var *distributionKey)
{
	ListCell *varClauseCell = NULL;
	int partitionColumnReferenceCount = 0;

	/* make sure partition column is used only once in the quals */
	List *varClauseList = pull_var_clause_default(quals);
	foreach(varClauseCell, varClauseList)
	{
		Var *column = (Var *) lfirst(varClauseCell);
		if (equal(column, distributionKey))
		{
			partitionColumnReferenceCount++;

			if (partitionColumnReferenceCount > 1)
			{
				return true;
			}
		}
	}

	return false;
}


/*
 * ConjunctionContainsColumnFilter returns true if the query contains an exact
 * match (equal) expression on the provided column. The function returns true only
 * if the match expression has an AND relation with the rest of the expression tree.
 *
 * If the conjuction contains column filter which is const, distributionKeyValue is set.
 */
static bool
ConjunctionContainsColumnFilter(Node *node, Var *column, Node **distributionKeyValue)
{
	if (node == NULL)
	{
		return false;
	}

	if (IsA(node, OpExpr))
	{
		OpExpr *opExpr = (OpExpr *) node;
		bool distKeyInSimpleOpExpression =
			DistKeyInSimpleOpExpression((Expr *) opExpr, column, distributionKeyValue);

		if (!distKeyInSimpleOpExpression)
		{
			return false;
		}

		return OperatorImplementsEquality(opExpr->opno);
	}
	else if (IsA(node, BoolExpr))
	{
		BoolExpr *boolExpr = (BoolExpr *) node;
		List *argumentList = boolExpr->args;
		ListCell *argumentCell = NULL;


		/*
		 * We do not descend into boolean expressions other than AND.
		 * If the column filter appears in an OR clause, we do not
		 * consider it even if it is logically the same as a single value
		 * comparison (e.g. `<column> = <Const> OR false`)
		 */
		if (boolExpr->boolop != AND_EXPR)
		{
			return false;
		}

		foreach(argumentCell, argumentList)
		{
			Node *argumentNode = (Node *) lfirst(argumentCell);

			if (ConjunctionContainsColumnFilter(argumentNode, column,
												distributionKeyValue))
			{
				return true;
			}
		}
	}

	return false;
}


/*
 * DistKeyInSimpleOpExpression checks whether given expression is a simple operator
 * expression with either (dist_key = param) or (dist_key = const). Note that the
 * operands could be in the reverse order as well.
 *
 * When a const is found, distributionKeyValue is set.
 */
static bool
DistKeyInSimpleOpExpression(Expr *clause, Var *distColumn, Node **distributionKeyValue)
{
	Param *paramClause = NULL;
	Const *constantClause = NULL;

	Var *columnInExpr = NULL;

	Node *leftOperand;
	Node *rightOperand;
	if (!BinaryOpExpression(clause, &leftOperand, &rightOperand))
	{
		return false;
	}

	if (IsA(rightOperand, Param) && IsA(leftOperand, Var))
	{
		paramClause = (Param *) rightOperand;
		columnInExpr = (Var *) leftOperand;
	}
	else if (IsA(leftOperand, Param) && IsA(rightOperand, Var))
	{
		paramClause = (Param *) leftOperand;
		columnInExpr = (Var *) rightOperand;
	}
	else if (IsA(rightOperand, Const) && IsA(leftOperand, Var))
	{
		constantClause = (Const *) rightOperand;
		columnInExpr = (Var *) leftOperand;
	}
	else if (IsA(leftOperand, Const) && IsA(rightOperand, Var))
	{
		constantClause = (Const *) leftOperand;
		columnInExpr = (Var *) rightOperand;
	}
	else
	{
		return false;
	}

	if (paramClause && paramClause->paramkind != PARAM_EXTERN)
	{
		/* we can only handle param_externs */
		return false;
	}
	else if (constantClause && constantClause->constisnull)
	{
		/* we can only handle non-null constants */
		return false;
	}

	/* at this point we should have the columnInExpr */
	Assert(columnInExpr);
	bool distColumnExists = equal(distColumn, columnInExpr);
	if (distColumnExists && constantClause != NULL &&
		distColumn->vartype == constantClause->consttype &&
		*distributionKeyValue == NULL)
	{
		/* if the vartypes do not match, let shard pruning handle it later */
		*distributionKeyValue = (Node *) copyObject(constantClause);
	}
	else if (paramClause != NULL)
	{
		*distributionKeyValue = (Node *) copyObject(paramClause);
	}

	return distColumnExists;
}
