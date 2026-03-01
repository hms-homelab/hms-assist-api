/**
 * Unit tests for VectorSearchService.
 *
 * Tests:
 *   - toVectorLiteral(): correct pgvector literal formatting for empty, single,
 *     multi, and negative-value vectors
 *   - entityCount() is virtual: mock subclass can override it (proves pqxx
 *     exec1() fix doesn't break the virtual dispatch contract)
 *
 * No real PostgreSQL or network connection required.
 */

#include "services/VectorSearchService.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using ::testing::Return;


// ─── Mock ─────────────────────────────────────────────────────────────────────

namespace {

class MockVectorSearchService : public VectorSearchService {
public:
    MockVectorSearchService() : VectorSearchService("") {}
    MOCK_METHOD(int, entityCount, (), (override));
    MOCK_METHOD(std::vector<EntityMatch>, search,
                (const std::vector<float>&, float, int), (override));
};

} // namespace


// ─── toVectorLiteral ──────────────────────────────────────────────────────────

TEST(VectorSearchServiceTest, ToVectorLiteral_Empty) {
    std::string result = VectorSearchService::toVectorLiteral({});
    EXPECT_EQ(result, "[]");
}

TEST(VectorSearchServiceTest, ToVectorLiteral_Single) {
    std::string result = VectorSearchService::toVectorLiteral({1.5f});
    EXPECT_EQ(result, "[1.5]");
}

TEST(VectorSearchServiceTest, ToVectorLiteral_Multi) {
    std::vector<float> vec = {1.0f, 2.0f, 3.0f};
    std::string result = VectorSearchService::toVectorLiteral(vec);
    // Should start with '[', end with ']', contain commas
    EXPECT_EQ(result.front(), '[');
    EXPECT_EQ(result.back(),  ']');
    EXPECT_NE(result.find(','), std::string::npos);
    EXPECT_NE(result.find("1"), std::string::npos);
    EXPECT_NE(result.find("2"), std::string::npos);
    EXPECT_NE(result.find("3"), std::string::npos);
}

TEST(VectorSearchServiceTest, ToVectorLiteral_NegativeValues) {
    std::vector<float> vec = {-0.5f, 0.0f, 0.5f};
    std::string result = VectorSearchService::toVectorLiteral(vec);
    EXPECT_EQ(result.front(), '[');
    EXPECT_EQ(result.back(),  ']');
    EXPECT_NE(result.find('-'), std::string::npos);
}

TEST(VectorSearchServiceTest, ToVectorLiteral_768Dims_HasCorrectFormat) {
    std::vector<float> vec(768, 0.1f);
    std::string result = VectorSearchService::toVectorLiteral(vec);
    EXPECT_EQ(result.front(), '[');
    EXPECT_EQ(result.back(),  ']');
    // 768 elements → 767 commas
    size_t commas = std::count(result.begin(), result.end(), ',');
    EXPECT_EQ(commas, 767u);
}


// ─── entityCount virtual dispatch ─────────────────────────────────────────────

TEST(VectorSearchServiceTest, EntityCount_MockReturnsExpected) {
    MockVectorSearchService svc;
    EXPECT_CALL(svc, entityCount()).WillOnce(Return(1115));
    EXPECT_EQ(svc.entityCount(), 1115);
}

TEST(VectorSearchServiceTest, EntityCount_MockReturnsZero) {
    MockVectorSearchService svc;
    EXPECT_CALL(svc, entityCount()).WillOnce(Return(0));
    EXPECT_EQ(svc.entityCount(), 0);
}

TEST(VectorSearchServiceTest, EntityCount_CalledTwice_ReturnsDifferentValues) {
    MockVectorSearchService svc;
    EXPECT_CALL(svc, entityCount())
        .WillOnce(Return(100))
        .WillOnce(Return(101));
    EXPECT_EQ(svc.entityCount(), 100);
    EXPECT_EQ(svc.entityCount(), 101);
}
