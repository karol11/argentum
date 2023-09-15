#include <iostream>
#include <iomanip>

#include <list>
#include "utils/fake-gunit.h"

namespace testing {

TestRegRecord* tests = nullptr;
int failed_count;
bool current_failed;
TestRegRecord::TestRegRecord(const char* name, Test* (*fn)())
	: name(name), fn(fn), next(tests)
{
	tests = this;
}

void Test::SetUp() {}
void Test::Run() {}
void Test::TearDown() {}

TestStream& TestStream::operator<< (nullptr_t) {
	if (on)
		std::cerr << "null";
	return *this;
}

TestStream& TestStream::operator<< (bool v) {
	if (on)
		std::cerr << (v ? "true" : "false");
	return *this;
}

TestStream::~TestStream() {
	if (on) {
		std::cerr << std::endl << std::flush;
		current_failed = true;
		if (severe)
			exit(-1);
	}
}

}  // namespace testing

int main() {
	::testing::failed_count = 0;
	std::list<testing::TestRegRecord*> tests;
	for (testing::TestRegRecord* trr = testing::tests; trr; trr = trr->next)
		tests.push_front(trr);
	std::cout << "Total " << tests.size() << " tests to run." << std::endl;
	int test_no = 0;
	for (auto trr : tests) {
		std::cout << "Test["<< std::setw(3) << std::setfill('0') <<  (++test_no) << "]: "
		  << trr->name << std::endl;
		::testing::Test* t = trr->fn();
		::testing::current_failed = false;
		t->SetUp();
		t->Run();
		t->TearDown();
		delete t;
		if (::testing::current_failed)
			::testing::failed_count++;
	}
	std::cout << (::testing::failed_count > 0 ? "failed" : "passed") << std::endl;
	return ::testing::failed_count;
}
