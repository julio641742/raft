#include "../lib/cluster.h"
#include "../lib/runner.h"

TEST_MODULE(membership);

/******************************************************************************
 *
 * Fixture
 *
 *****************************************************************************/

struct fixture
{
    FIXTURE_CLUSTER;
    struct raft_change req;
};

static char *cluster_n[] = {"3", "4", "5", NULL};

static MunitParameterEnum _params[] = {
    {CLUSTER_N_PARAM, cluster_n},
    {NULL, NULL},
};

static void *setup(const MunitParameter params[], void *user_data)
{
    struct fixture *f = munit_malloc(sizeof *f);
    (void)user_data;
    SETUP_CLUSTER(0);
    CLUSTER_BOOTSTRAP;
    CLUSTER_RANDOMIZE;
    CLUSTER_START;
    CLUSTER_STEP_UNTIL_HAS_LEADER(10000);
    return f;
}

static void tear_down(void *data)
{
    struct fixture *f = data;
    TEAR_DOWN_CLUSTER;
    free(f);
}

/******************************************************************************
 *
 * Tests
 *
 *****************************************************************************/

TEST_SUITE(add);
TEST_SETUP(add, setup);
TEST_TEAR_DOWN(add, tear_down);

TEST_CASE(add, non_voting, _params)
{
    struct fixture *f = data;
    const struct raft_server *server;
    struct raft *raft;

    (void)params;

    CLUSTER_ADD(&f->req);
    CLUSTER_STEP_UNTIL_APPLIED(CLUSTER_N, 2, 2000);

    raft = CLUSTER_RAFT(CLUSTER_LEADER);

    server = &raft->configuration.servers[CLUSTER_N - 1];
    munit_assert_int(server->id, ==, CLUSTER_N);

    return MUNIT_OK;
}

TEST_CASE(add, voting, _params)
{
    struct fixture *f = data;
    const struct raft_server *server;
    struct raft *raft;

    (void)params;

    CLUSTER_ADD(&f->req);
    CLUSTER_STEP_UNTIL_APPLIED(CLUSTER_N, 2, 2000);

    /* Then promote it. */
    CLUSTER_PROMOTE(&f->req);

    CLUSTER_STEP_UNTIL_APPLIED(CLUSTER_N, 3, 2000);

    raft = CLUSTER_RAFT(CLUSTER_LEADER);

    server = &raft->configuration.servers[CLUSTER_N - 1];
    munit_assert_true(server->voting);

    return MUNIT_OK;
}

TEST_SUITE(remove);
TEST_SETUP(remove, setup);
TEST_TEAR_DOWN(remove, tear_down);

TEST_CASE(remove, voting, _params)
{
    struct fixture *f = data;
    struct raft *raft;
    int rv;

    (void)params;

    raft = CLUSTER_RAFT(CLUSTER_LEADER);

    rv = raft_remove(raft, &f->req, CLUSTER_LEADER % CLUSTER_N + 1, NULL);
    munit_assert_int(rv, ==, 0);

    CLUSTER_STEP_UNTIL_APPLIED(CLUSTER_LEADER, 2, 2000);

    munit_assert_int(raft->configuration.n, ==, CLUSTER_N - 1);

    return 0;
}
