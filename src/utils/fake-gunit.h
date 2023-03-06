#ifndef GTEST_H
#define GTEST_H

#include <iostream>

namespace testing {

class Test;

struct TestRegRecord {
	const char* name;
	TestRegRecord* next;
	Test* (*fn)();
	TestRegRecord(const char* name, Test* (*fn)());
};

class Test {
public:
	virtual void SetUp();
	virtual void Run();
	virtual void TearDown();
};

struct TestStream
{
	TestStream(bool on, bool severe) : on(on), severe(severe) {}
	TestStream(const TestStream& src) : on(src.on), severe(src.severe) { src.on = false; }
	~TestStream();
	template<typename T>
	TestStream& operator<< (const T& v) {
		if (on)
			std::cerr << v;
		return *this;
	}
	TestStream& operator<< (nullptr_t);
	TestStream& operator<< (bool v);
	mutable bool on;
	bool severe;
};

}  // namespace testing

#define TEST_STR_(S) #S
#define TEST_STR(S) TEST_STR_(S)
#define TEST_CLASS_NAME(CASE, TST) CASE##_##TST##_Test

#define TEST_(CASE, TST, BASE)                                            \
	class TEST_CLASS_NAME(CASE, TST) : public BASE {                      \
	public:                                                               \
		static Test* Create_() { return new TEST_CLASS_NAME(CASE, TST); } \
		void Run() override;                                              \
	};                                                                    \
	::testing::TestRegRecord CASE##_##TST##reg(                           \
			TEST_STR(TEST_CLASS_NAME(CASE, TST)),                         \
			TEST_CLASS_NAME(CASE, TST)::Create_);                         \
	void TEST_CLASS_NAME(CASE, TST)::Run()

#define TEST(CASE, TST) TEST_(CASE, TST, ::testing::Test)
#define TEST_F(CASE, TST) TEST_(CASE, TST, CASE)

#define TEST_OP_EQ_ ==
#define TEST_OP_NE_ !=
#define TEST_OP_LT_ <
#define TEST_OP_LE_ <=

#define TEST_OP_(A, B, OP, SEVERE)  \
	[&]{                       \
		const auto& a_ = A;   \
		const auto& b_ = B;   \
		return testing::TestStream(!(a_ OP b_), SEVERE) << "[E]"  << __FILE__ << ":" << __LINE__ << " "<< TEST_STR(A) << " (" << a_ << TEST_STR(OP) << b_ << ") " << TEST_STR(B); \
	}()

#define ASSERT_EQ(A, B) TEST_OP_(A, B, TEST_OP_EQ_, true)
#define ASSERT_NE(A, B) TEST_OP_(A, B, TEST_OP_NE_, true)
#define ASSERT_LT(A, B) TEST_OP_(A, B, TEST_OP_LT_, true)
#define ASSERT_LE(A, B) TEST_OP_(A, B, TEST_OP_LE_, true)
#define ASSERT_GT(A, B) TEST_OP_(B, A, TEST_OP_LT_, true)
#define ASSERT_GE(A, B) TEST_OP_(A, B, TEST_OP_LE_, true)
#define ASSERT_FALSE(A) TEST_OP_(A, false, TEST_OP_EQ_, true)
#define ASSERT_TRUE(A) TEST_OP_(A, true, TEST_OP_EQ_, true)

#define EXPECT_EQ(A, B) TEST_OP_(A, B, TEST_OP_EQ_, false)
#define EXPECT_NE(A, B) TEST_OP_(A, B, TEST_OP_NE_, false)
#define EXPECT_LT(A, B) TEST_OP_(A, B, TEST_OP_LT_, false)
#define EXPECT_LE(A, B) TEST_OP_(A, B, TEST_OP_LE_, false)
#define EXPECT_GT(A, B) TEST_OP_(B, A, TEST_OP_LT_, false)
#define EXPECT_GE(A, B) TEST_OP_(A, B, TEST_OP_LE_, false)
#define EXPECT_FALSE(A) TEST_OP_(A, false, TEST_OP_EQ_, false)
#define EXPECT_TRUE(A) TEST_OP_(A, true, TEST_OP_EQ_, false)

#endif  // GTEST
