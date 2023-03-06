/*
Copyright 2018 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include <functional>
#include <iterator>
#include <vector>
#include <string>

#include "utils/fake-gunit.h"
#include "ltm/ltm.h"

namespace ltm {

struct LtmTester{
	static void check(const Object *c, uintptr_t flags, uintptr_t weak_flags = 0) {
		if (!c) {
			ASSERT_EQ(flags, 0);
			ASSERT_EQ(weak_flags, 0);
		} else if (c->counter & Object::WEAKLESS) {
			ASSERT_EQ(c->counter, flags);
			ASSERT_EQ(weak_flags, 0);
		} else {
			ASSERT_EQ(c->weak_block->org_counter, flags);
			ASSERT_EQ(c->weak_block->counter, weak_flags);
			ASSERT_EQ(c->weak_block->target, c);
		}
	}
	template<typename T>
	static void check(const pin<T>& c, uintptr_t flags, uintptr_t weak_flags = 0) {
		check(c.operator->(), flags, weak_flags);
	}
	template<typename T>
	static void check(const own<T>& c, uintptr_t flags, uintptr_t weak_flags = 0) {
		check(c.operator->(), flags, weak_flags);
	}
	template<typename T>
	static void check(const weak<T>& c, uintptr_t flags, uintptr_t weak_flags = 0) {
		check(c.operator->(), flags, weak_flags);
	}
	enum {
		WEAKLESS = Object::WEAKLESS,
		OWNED = Object::OWNED,
		SHARED = Object::SHARED,
		COUNTER_STEP = Object::COUNTER_STEP,
	};
};

}  // namespace ltm

namespace {

using std::vector;
using std::string;
using std::function;
using ltm::Object;
using ltm::own;
using ltm::pin;
using ltm::weak;
using ltm::iweak;
using ltm::mc;
using ltm::Proxy;
using lt = ltm::LtmTester;

struct PageItem : public Object {
	LTM_COPYABLE(PageItem);
};

struct Point : public PageItem {
	int x = 0, y = 0;
	LTM_COPYABLE(Point);
};

bool type_test_fn(own<PageItem>& p) {
	return p;
}

TEST(LTM, TypeTest) {
	own<PageItem> p = new Point(); // Init pointer to base with derived instance.
	p = new Point();               // Assign derived instance.
	auto xx = p.cast<Point>()->x;  // Upcast
	EXPECT_EQ(xx, 0);
	own<Point> pt;
	EXPECT_EQ(type_test_fn(pt), false);
	EXPECT_EQ(type_test_fn(own<Point>()), false);
	EXPECT_EQ(type_test_fn(own<Point>::make()), true);
}


struct Node : Object {
	char c;
	own<Node> left, right;

	Node(char c, pin<Node> left = nullptr, pin<Node> right = nullptr)
		: c(c), left(left), right(right) {}
	LTM_COPYABLE(Node);
};


TEST(LTM, ConstructionAndImplicitConversions) {
	own<Point> a = new Point;
	lt::check(a, lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);

	pin<Point> a_temp = a;
	lt::check(a, 2 * lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	ASSERT_EQ(a_temp, a);

	own<PageItem> b = a_temp;
	lt::check(a, 2 * lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	ASSERT_EQ(a_temp, a);
	ASSERT_NE(b, a);
	lt::check(b, lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);

	weak<PageItem> w = a;
	lt::check(a, 2 * lt::COUNTER_STEP + lt::OWNED, 2 * lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	ASSERT_EQ(w.pinned(), a);
}

TEST(LTM, AutoConstruction) {
	auto a = own<PageItem>::make<Point>();
	auto weak_to_a = a.weaked();
	auto pinned_a = a.pinned();
	auto x = a;
	ASSERT_NE(x, a);
	ASSERT_EQ(pinned_a, a);
	ASSERT_EQ(weak_to_a, a);
}

struct XrefNode : Object {
	char c;
	own<XrefNode> left, right;
	weak<XrefNode> xref;

	XrefNode(char c, XrefNode* left = nullptr, XrefNode* right = nullptr)
		: c(c), left(left), right(right)
	{
		if (left)
			left->xref = this;
		if (right)
			right->xref = this;
	}
	LTM_COPYABLE(XrefNode);
};

void check_xref_tree(const own<XrefNode>& root) {
	lt::check(root,
		lt::COUNTER_STEP + lt::OWNED,
		3 * lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	lt::check(root->left, lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	lt::check(root->right, lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	ASSERT_EQ(root->left->xref.pinned(), root);
	ASSERT_EQ(root->right->xref.pinned(), root);
}

TEST(LTM, CopyOps) {
	own<XrefNode> root = new XrefNode('a', new XrefNode('b'), new XrefNode('c'));
	check_xref_tree(root);

	own<XrefNode> r2 = root;
	check_xref_tree(root);
	check_xref_tree(r2);

	r2->left->xref = r2->left;
	r2->right->xref = r2->left;

	lt::check(r2,
		lt::COUNTER_STEP + lt::OWNED,
		1 * lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	lt::check(r2->left,
		lt::COUNTER_STEP + lt::OWNED,
		3 * lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	lt::check(r2->right, lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	ASSERT_EQ(r2->left->xref.pinned(), r2->left);
	ASSERT_EQ(r2->right->xref.pinned(), r2->left);

	auto r3 = r2;
	lt::check(r2,
		lt::COUNTER_STEP + lt::OWNED,
		1 * lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	lt::check(r2->left,
		lt::COUNTER_STEP + lt::OWNED,
		3 * lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	lt::check(r2->right, lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	ASSERT_EQ(r2->left->xref.pinned(), r2->left);
	ASSERT_EQ(r2->right->xref.pinned(), r2->left);
	lt::check(r3, lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	lt::check(r3->left,
		lt::COUNTER_STEP + lt::OWNED,
		3 * lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	lt::check(r3->right, lt::COUNTER_STEP + lt::OWNED + lt::WEAKLESS);
	ASSERT_EQ(r3->left->xref.pinned(), r3->left);
	ASSERT_EQ(r3->right->xref.pinned(), r3->left);

	if (auto p = root->xref.pinned())  // access fields by weak ptr with checking
		p->c = 'x';
}

struct NodeWithHandler : Object {
	own<NodeWithHandler> child;
	char c;
	function<void(int param)> handler = [](char) {};

	NodeWithHandler(char c, const pin<NodeWithHandler>& child = nullptr)
		: c(c), child(child)
	{}
	LTM_COPYABLE(NodeWithHandler);
};

TEST(LTM, WeakHandlers) {
	auto root = own<NodeWithHandler>::make('a', pin<NodeWithHandler>::make('b'));
	root->handler = [ctx = root->child.weaked()](char param) {
		if (auto me = ctx.pinned())
			me->c = param;
	};
	root->handler('x');
	ASSERT_EQ(root->child->c, 'x');

	auto r2 = root;
	r2->handler('y');
	ASSERT_EQ(r2->child->c, 'y');

	r2->child = nullptr;
	r2->handler('z');  // does nothing.
}

struct IPaintable : Object {
	virtual void paint(Point* p) = 0;
	virtual int get_width() = 0;
};

TEST(LTM, ProxyAdapter) {
	struct PaintablePoint : Proxy<IPaintable> {
		weak<Point> me;
		PaintablePoint(pin<Point> me) : me(me) { make_shared(); }
		void paint(Point* p) override {
			// implement the IPaintable interface using `me`
		}
		int get_width() override {
			auto p = me.pinned();
			return p ? p->x : 0;
		}
	};

	auto p = own<Point>::make();
	p->x = 42;
	weak<IPaintable> pp = new PaintablePoint(p);  // pp is weak but it upholds PaintablePoint.
	auto x = pp.pinned();
	ASSERT_NE(x, nullptr);
	ASSERT_EQ(x->get_width(), 42);
	p = nullptr;
	ASSERT_EQ(x->get_width(), 0);
}

struct IInterface {
	virtual int method1() = 0;
	virtual void method2(int param) = 0;
};

class Impl : public Object, public IInterface {
	int field1;
 public:
	Impl(int i) : field1(i) {}
	int method1() override { return field1; }
	void method2(int param) override { field1 = param; }
	LTM_COPYABLE(Impl);
};

TEST(LTM, InterafaceTest) {
	auto c = own<Impl>::make(42);
	iweak<IInterface> w = c.weaked();
	if (auto i = w.pinned())
		i->method2(11);
	ASSERT_EQ(c->method1(), 11);
}

}  // namespace
