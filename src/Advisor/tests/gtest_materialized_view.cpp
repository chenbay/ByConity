#include <Advisor/Rules/MaterializedViewAdvise.h>

#include <Advisor/AdvisorContext.h>
#include <Parsers/formatAST.h>
#include <Optimizer/CardinalityEstimate/CardinalityEstimator.h>
#include <Optimizer/PlanNodeSearcher.h>
#include <QueryPlan/PlanPrinter.h>

#include <iostream>
#include <Advisor/tests/gtest_workload_test.h>
#include <Optimizer/tests/gtest_base_tpcds_plan_test.h>
#include <gtest/gtest.h>

namespace DB
{
class MaterializedViewAdviseTest : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        tester = std::make_shared<BaseTpcdsPlanTest>(BasePlanTest::getDefaultOptimizerSettings());
        tester->execute("CREATE TABLE IF NOT EXISTS emps("
                        "  empid UInt32 not null,"
                        "  deptno UInt32 not null,"
                        "  name Nullable(String),"
                        "  salary Nullable(Float64),"
                        "  commission Nullable(UInt32)"
                        ") ENGINE=Memory();");
    }

    class TestMaterializedViewAdvisor : public MaterializedViewAdvisor
    {
    public:
        explicit TestMaterializedViewAdvisor(): MaterializedViewAdvisor(false)
        {
            ADVISE_MIN_TABLE_SIZE = 0;
        }
    };

    static WorkloadAdvises getAdvises(std::vector<std::string> sql_list)
    {
        auto context = tester->createQueryContext();
        std::vector<std::string> sqls(sql_list);
        ThreadPool query_thread_pool{std::min<size_t>(size_t(context->getSettingsRef().max_threads), sqls.size())};
        WorkloadQueries queries = WorkloadQuery::build(std::move(sqls), context, query_thread_pool);
        WorkloadTables tables(context);
        AdvisorContext advisor_context = AdvisorContext::buildFrom(context, tables, queries, query_thread_pool);
        auto advises = TestMaterializedViewAdvisor().analyze(advisor_context);
        return advises;
    }

    static std::shared_ptr<BaseTpcdsPlanTest> tester;
};

std::shared_ptr<BaseTpcdsPlanTest> MaterializedViewAdviseTest::tester;

namespace
{
    void printAdvises(const WorkloadAdvises & advises)
    {
        for (const auto & advise : advises)
        {
            std::cout << advise->getAdviseType() << '\t' << advise->getTable().database << "." << advise->getTable().table << '\t'
                      << advise->getOptimizedValue() << '\t' << advise->getBenefit() << std::endl;
        }
    }

    bool isAgg(PlanNodeBase & node)
    {
        return node.getStep()->getType() == IQueryPlanStep::Type::Aggregating;
    }

}

#define EXPECT_CONTAINS(text, substr) EXPECT_NE(text.find(substr), std::string::npos)

#define EXPECT_NOT_CONTAINS(text, substr) EXPECT_EQ(text.find(substr), std::string::npos)

TEST_F(MaterializedViewAdviseTest, TestMaterializedViewBuilder)
{
    WorkloadQueryPtr query = WorkloadQuery::build("select empid as id, deptno+1 as d from emps group by empid, deptno having id = 1",
                                                  tester->getSessionContext());
    auto root = PlanNodeSearcher::searchFrom(query->getPlan()->getPlanNode()).where(isAgg).findFirst().value(); // MergingAgg not supported
    auto candidate = MaterializedViewCandidate::from(root);
    ASSERT_TRUE(candidate.has_value());
    std::string sql = candidate.value().toSQL();
    std::cout << sql << std::endl;
    EXPECT_CONTAINS(sql, "FROM " + tester->getDatabaseName() + ".emps");
    EXPECT_CONTAINS(sql, "WHERE empid = 1");
    EXPECT_CONTAINS(sql, "GROUP BY");
}

TEST_F(MaterializedViewAdviseTest, TestMaterializedViewBuilderComplicated)
{
    WorkloadQueryPtr query = WorkloadQuery::build(
        "select id*2, sum(d+1) from (select empid+1 as id, deptno-1 as d from emps where name is not null) group by id",
        tester->getSessionContext());
    auto root = PlanNodeSearcher::searchFrom(query->getPlan()->getPlanNode()).where(isAgg).findFirst().value(); // MergingAgg not supported
    auto candidate = MaterializedViewCandidate::from(root);
    ASSERT_TRUE(candidate.has_value());
    std::string sql = candidate.value().toSQL();
    std::cout << sql << std::endl;
    EXPECT_CONTAINS(sql, "FROM " + tester->getDatabaseName() + ".emps");
    EXPECT_CONTAINS(sql, "GROUP BY");
    ASSERT_NO_THROW(tester->execute(sql));
}

TEST_F(MaterializedViewAdviseTest, TestMaterializedViewBuilderFail)
{
    WorkloadQueryPtr query = WorkloadQuery::build("select e1.empid from emps e1, emps e2 where e1.deptno=e2.deptno",
                                                  tester->getSessionContext());
    auto candidate = MaterializedViewCandidate::from(query->getPlan()->getPlanNode());
    ASSERT_FALSE(candidate.has_value());
}

TEST_F(MaterializedViewAdviseTest, TestMaterializedViewAdviseSimple)
{
    auto advises = getAdvises({
                                  "select d1.* from date_dim d1, date_dim d2 where d1.d_date_sk != d2.d_date_sk and d1.d_week_seq=1",
                                  "select d2.* from date_dim d1, date_dim d2 where d1.d_date_sk != d2.d_date_sk and d2.d_week_seq=1 and d1.d_month_seq > 1"
                              });
    // only recommend d_week_seq = 1 because appeared twice
    ASSERT_EQ(advises.size(), 1);
    printAdvises(advises);
    EXPECT_CONTAINS(advises.front()->getOptimizedValue(), "d_week_seq = 1");
}

TEST_F(MaterializedViewAdviseTest, TestMaterializedViewFilterAndProject)
{
    auto advises = getAdvises({
                                  "select d_month_seq - 1, d_date_sk + 1 from date_dim where d_week_seq = 1",
                                  "select d_date_sk + 1, d_month_seq - 1 from date_dim where d_week_seq = 1"
                              });
    printAdvises(advises);
    ASSERT_EQ(advises.size(), 1);
    EXPECT_CONTAINS(advises.front()->getOptimizedValue(), "d_date_sk + 1");
    EXPECT_CONTAINS(advises.front()->getOptimizedValue(), "d_month_seq - 1");
}

TEST_F(MaterializedViewAdviseTest, TestMaterializedViewFilterAndProject2)
{
    auto advises = getAdvises({
                                  "select d_month_seq - 1, d_date_sk + 1 from date_dim where d_week_seq = 1",
                                  "select d_month_seq - 1, d_date_sk + 1 from date_dim where d_week_seq = 1",
                                  "select d_month_seq, d_date_sk from date_dim where d_week_seq = 1"
                              });
    printAdvises(advises);
    // do not recommend parent
    ASSERT_EQ(advises.size(), 1);
    EXPECT_CONTAINS(advises.front()->getOptimizedValue(), "d_week_seq = 1");
    EXPECT_NOT_CONTAINS(advises.front()->getOptimizedValue(), "d_date_sk + 1");
}

TEST_F(MaterializedViewAdviseTest, TestMaterializedViewGroupBy)
{
    auto advises = getAdvises({
                                  "select count(distinct d_month_seq), sum(d_date_sk) from date_dim group by d_week_seq",
                                  "select count(distinct d_month_seq), sum(d_date_sk), sum(d_date_sk) + 1 from date_dim group by d_week_seq"
                              });
    printAdvises(advises);
    ASSERT_EQ(advises.size(), 1);
    EXPECT_CONTAINS(advises.front()->getOptimizedValue(), "sum(d_date_sk)");
}

TEST_F(MaterializedViewAdviseTest, TestMaterializedViewGroupByAndProjectAndFilter)
{
    auto advises = getAdvises({
                                  "select count(distinct d_month_seq), sum(d_date_sk) from date_dim where d_date_sk > 1 group by d_week_seq",
                                  "select count(distinct d_month_seq), sum(d_date_sk) from date_dim where d_date_sk > 1 group by d_week_seq"
                              });
    printAdvises(advises);
    ASSERT_EQ(advises.size(), 1);
    EXPECT_CONTAINS(advises.front()->getOptimizedValue(), "sum(d_date_sk)");
    EXPECT_CONTAINS(advises.front()->getOptimizedValue(), "d_date_sk > 1");
}

TEST_F(MaterializedViewAdviseTest, TestMaterializedViewDistinctAgg)
{
    auto advises = getAdvises({
                                  "select d_week_seq, sum(distinct d_month_seq) from date_dim where d_date_sk > 1 group by d_week_seq",
                                  "select d_week_seq, sum(distinct d_month_seq) from date_dim where d_date_sk > 1 group by d_week_seq"
                              });
    printAdvises(advises);
    ASSERT_EQ(advises.size(), 1);
    EXPECT_CONTAINS(advises.front()->getOptimizedValue(), "sumDistinct(d_month_seq)");
    EXPECT_CONTAINS(advises.front()->getOptimizedValue(), "d_date_sk > 1");
}

TEST_F(MaterializedViewAdviseTest, TestMaterializedViewMergingNoAgg)
{
    auto advises = getAdvises({
                                  "select d_week_seq from date_dim where d_year = 2000",
                                  "select d_week_seq from date_dim where d_year = 2000",
                                  "select d_date_sk from date_dim where d_year = 2000",
                                  "select d_date_sk from date_dim where d_year = 2000"
                              });
    printAdvises(advises);
    ASSERT_EQ(advises.size(), 1);
    EXPECT_CONTAINS(advises.front()->getOptimizedValue(), "d_week_seq");
    EXPECT_CONTAINS(advises.front()->getOptimizedValue(), "d_date_sk");
}


TEST_F(MaterializedViewAdviseTest, TestMaterializedViewCaseWhen)
{
    const char * sql_template = "select d_week_seq, cast(case d_month_seq "
                                "     when 1 then 'D_MONTH_SEQ_1'"
                                "     when 2 then 'D_MONTH_SEQ_2'"
                                "     when 3 then 'D_MONTH_SEQ_3'"
                                "     else {0} end as Nullable(String)) as d_month_seq_1,"
                                " count(cast(case d_month_seq "
                                "     when 1 then 'D_MONTH_SEQ_1'"
                                "     when 2 then 'D_MONTH_SEQ_2'"
                                "     when 3 then 'D_MONTH_SEQ_3'"
                                "     else {0} end as Nullable(String))) as d_month_seq_count"
                                " from date_dim"
                                " where {1}"
                                " group by d_week_seq, cast(case d_month_seq "
                                "     when 1 then 'D_MONTH_SEQ_1'"
                                "     when 2 then 'D_MONTH_SEQ_2'"
                                "     when 3 then 'D_MONTH_SEQ_3'"
                                "     else {0} end as Nullable(String))";
    std::string sql = fmt::format(sql_template, "'D_MONTH_SEQ_OTHER'", "d_date_sk > 2450000");
    std::string sql2 = fmt::format(sql_template, "'OTHER'", "d_date_sk > 2450000 and d_date_sk < 2460000");

    auto advises = getAdvises({sql, sql, sql2, sql2});
    printAdvises(advises);
    EXPECT_GE(advises.size(), 1);
}

TEST_F(MaterializedViewAdviseTest, TestTPCDSQ1)
{
    std::string sql = tester->loadQuery("q1").sql.front().first;
    auto advises = getAdvises({sql});

    // 4 candidates, the first two are recommended based on cost
    // select d_date_sk, d_year from tpcds.date_dim where d_year = 2000
    // select s_state, s_store_sk from tpcds.store where s_state = 'TN'
    // select s_store_sk from tpcds.store where s_state = 'TN'
    // select s_store_sk from tpcds.store where s_state = 'TN' group by s_store_sk
    printAdvises(advises);
    ASSERT_EQ(advises.size(), 2);
    std::sort(advises.begin(), advises.end(), [](const auto & left, const auto & right) {
        return left->getBenefit() > right->getBenefit();
    });
    auto date_dim_advise = advises[0];
    QualifiedTableName date_dim{tester->getDatabaseName(), "date_dim"};
    EXPECT_EQ(date_dim_advise->getTable(), date_dim);
    EXPECT_CONTAINS(date_dim_advise->getOptimizedValue(), "FROM " + tester->getDatabaseName() + ".date_dim WHERE d_year = 2000");
    EXPECT_CONTAINS(date_dim_advise->getOptimizedValue(), "d_year");
    EXPECT_CONTAINS(date_dim_advise->getOptimizedValue(), "d_date_sk");
    auto store_advise = advises[1];
    QualifiedTableName store{tester->getDatabaseName(), "store"};
    EXPECT_EQ(store_advise->getTable(), store);
    EXPECT_CONTAINS(store_advise->getOptimizedValue(), "FROM " + tester->getDatabaseName() + ".store WHERE s_state = 'TN'");
    EXPECT_CONTAINS(store_advise->getOptimizedValue(), "s_store_sk");
    EXPECT_CONTAINS(store_advise->getOptimizedValue(), "s_state");

    auto advises_twice = getAdvises({sql, sql});
    ASSERT_EQ(advises.size(), 2);
    EXPECT_EQ(advises_twice[0]->getOptimizedValue(), advises[0]->getOptimizedValue());
    EXPECT_EQ(advises_twice[1]->getOptimizedValue(), advises[1]->getOptimizedValue());
}

TEST_F(MaterializedViewAdviseTest, TestTPCDSQ6)
{
    std::string sql = tester->loadQuery("q6").sql.front().first;
    auto advises = getAdvises({sql});
    EXPECT_EQ(advises.size(), 0);

    auto advises_twice = getAdvises({sql, sql});
    printAdvises(advises_twice);
    ASSERT_EQ(advises_twice.size(), 2);
    std::sort(advises_twice.begin(), advises_twice.end(), [](const auto & left, const auto & right) {
        return left->getBenefit() > right->getBenefit();
    });
    auto item_advise = advises_twice[0];
    QualifiedTableName item{tester->getDatabaseName(), "item"};
    EXPECT_EQ(item_advise->getTable(), item);
    EXPECT_CONTAINS(item_advise->getOptimizedValue(), "FROM " + tester->getDatabaseName() + ".item GROUP BY i_category");
    EXPECT_CONTAINS(item_advise->getOptimizedValue(), "i_category");
    EXPECT_CONTAINS(item_advise->getOptimizedValue(), "avgIf(i_current_price, 1)");
    auto date_dim_advise = advises_twice[1];
    QualifiedTableName date_dim{tester->getDatabaseName(), "date_dim"};
    EXPECT_EQ(date_dim_advise->getTable(), date_dim);
    EXPECT_CONTAINS(date_dim_advise->getOptimizedValue(), "FROM " + tester->getDatabaseName() +
                        ".date_dim WHERE (d_year = 2001) AND (d_moy = 1) GROUP BY d_month_seq");
    EXPECT_CONTAINS(date_dim_advise->getOptimizedValue(), "d_month_seq");
}

}
