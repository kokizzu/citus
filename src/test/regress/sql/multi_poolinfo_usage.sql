--
-- MULTI_POOLINFO_USAGE
--
-- Test pooler info logic
--
-- Test of ability to override host/port for a node
SET citus.shard_replication_factor TO 1;
SET citus.next_shard_id TO 20000000;

SELECT nodeid AS worker_1_id FROM pg_dist_node WHERE nodename = 'localhost' AND nodeport = :worker_1_port;
\gset
SELECT nodeid AS worker_2_id FROM pg_dist_node WHERE nodename = 'localhost' AND nodeport = :worker_2_port;
\gset

CREATE TABLE lotsa_connections (id integer, name text);
SELECT create_distributed_table('lotsa_connections', 'id');

INSERT INTO lotsa_connections VALUES (1, 'user'), (2, 'user'), (3, 'user'), (4, 'user');

SELECT COUNT(*) FROM lotsa_connections;

-- put outright bad values
\set VERBOSITY terse
INSERT INTO pg_dist_poolinfo VALUES (:worker_1_id, 'host=failhost'),
                                    (:worker_2_id, 'port=9999');
\c

-- supress OS specific error message
DO $$
BEGIN
        BEGIN
			SELECT COUNT(*) FROM lotsa_connections;
        EXCEPTION WHEN OTHERS THEN
                IF SQLERRM LIKE 'connection to the remote node%%' THEN
                       RAISE 'failed to execute select';
                END IF;
        END;
END;
$$;

-- "re-route" worker one to node two and vice-versa
DELETE FROM pg_dist_poolinfo;
INSERT INTO pg_dist_poolinfo VALUES (:worker_1_id, 'port=' || :worker_2_port),
                                    (:worker_2_id, 'port=' || :worker_1_port);
\c

-- this fails because the shards of one worker won't exist on the other and shards
-- are still looked up using the node name, not the effective connection host
INSERT INTO lotsa_connections VALUES (1, 'user'), (2, 'user'), (3, 'user'), (4, 'user');

-- tweak poolinfo to use 127.0.0.1 instead of localhost; should work!
DELETE FROM pg_dist_poolinfo;
INSERT INTO pg_dist_poolinfo VALUES (:worker_1_id, 'host=127.0.0.1 port=' || :worker_1_port),
                                    (:worker_2_id, 'host=127.0.0.1 port=' || :worker_2_port);
\c

DELETE FROM lotsa_connections;
DROP TABLE lotsa_connections;

DELETE FROM pg_dist_poolinfo;
