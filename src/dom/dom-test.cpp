#include <limits>
#include "utils/fake-gunit.h"
#include "dom/dom.h"

namespace {

using ltm::pin;
using ltm::own;
using ltm::weak;
using ltm::Object;
using dom::DomItem;
using dom::Dom;
using dom::TypeInfo;
using dom::FieldInfo;
using dom::DomField;
using std::vector;
using dom::Kind;
using dom::CppField;

template<typename T>
void test_int() {
	auto dom = pin<Dom>::make();
	auto int_type = dom->mk_type(Kind::INT, sizeof(T));
	EXPECT_EQ(int_type->get_size(), sizeof(T));
	char data[sizeof(T)];
	int_type->init(data);
	EXPECT_EQ(int_type->get_int(data), 0);
	int_type->set_int(std::numeric_limits<T>::min(), data);
	EXPECT_EQ(int_type->get_int(data), std::numeric_limits<T>::min());
	int_type->set_int(std::numeric_limits<T>::max(), data);
	EXPECT_EQ(int_type->get_int(data), std::numeric_limits<T>::max());
	int_type->dispose(data);
}

TEST(Dom, Primitives) {
	test_int<int8_t>();
	test_int<int16_t>();
	test_int<int32_t>();
	test_int<int64_t>();
}

TEST(Dom, String) {
	auto dom = pin<Dom>::make();
	auto str_type = dom->mk_type(Kind::STRING);
	char* str_data = new char[str_type->get_size()];
	str_type->init(str_data);
	EXPECT_EQ(str_type->get_string(str_data), "");
	str_type->set_string("Hello", str_data);
	EXPECT_EQ(str_type->get_string(str_data), "Hello");
	str_type->dispose(str_data);
	delete[] str_data;
}

TEST(Dom, FixedArrays) {
	auto dom = pin<Dom>::make();
	auto str_type = dom->mk_type(Kind::STRING);
	auto array_type = dom->mk_type(Kind::FIX_ARRAY, 3, str_type);
	char* data = new char[array_type->get_size()];
	EXPECT_EQ(array_type->get_elements_count(data), 3);
	array_type->init(data);
	str_type->set_string("qwerty", array_type->get_element_ptr(0, data));
	str_type->copy(array_type->get_element_ptr(0, data), array_type->get_element_ptr(1, data));
	str_type->set_string("asdfg", array_type->get_element_ptr(2, data));
	EXPECT_EQ(str_type->get_string(array_type->get_element_ptr(0, data)), "qwerty");
	EXPECT_EQ(str_type->get_string(array_type->get_element_ptr(1, data)), "qwerty");
	EXPECT_EQ(str_type->get_string(array_type->get_element_ptr(2, data)), "asdfg");
	array_type->dispose(data);
	delete[] data;
}

TEST(Dom, VarArrays) {
	auto dom = pin<Dom>::make();
	auto str_type = dom->mk_type(Kind::STRING);
	auto array_type = dom->mk_type(Kind::VAR_ARRAY, 0, str_type);
	char* data = new char[array_type->get_size()];
	array_type->init(data);
	EXPECT_EQ(array_type->get_elements_count(data), 0);
	array_type->set_elements_count(2, data);
	EXPECT_EQ(array_type->get_elements_count(data), 2);
	EXPECT_EQ(str_type->get_string(array_type->get_element_ptr(0, data)), "");
	EXPECT_EQ(str_type->get_string(array_type->get_element_ptr(1, data)), "");
	str_type->set_string("qwerty", array_type->get_element_ptr(0, data));
	str_type->copy(array_type->get_element_ptr(0, data), array_type->get_element_ptr(1, data));
	EXPECT_EQ(str_type->get_string(array_type->get_element_ptr(0, data)), "qwerty");
	EXPECT_EQ(str_type->get_string(array_type->get_element_ptr(1, data)), "qwerty");
	array_type->set_elements_count(1, data);
	EXPECT_EQ(array_type->get_elements_count(data), 1);
	EXPECT_EQ(str_type->get_string(array_type->get_element_ptr(0, data)), "qwerty");
	array_type->set_elements_count(3, data);
	EXPECT_EQ(array_type->get_elements_count(data), 3);
	EXPECT_EQ(str_type->get_string(array_type->get_element_ptr(0, data)), "qwerty");
	EXPECT_EQ(str_type->get_string(array_type->get_element_ptr(1, data)), "");
	EXPECT_EQ(str_type->get_string(array_type->get_element_ptr(2, data)), "");
	str_type->set_string("asdfg", array_type->get_element_ptr(2, data));
	EXPECT_EQ(str_type->get_string(array_type->get_element_ptr(0, data)), "qwerty");
	EXPECT_EQ(str_type->get_string(array_type->get_element_ptr(1, data)), "");
	EXPECT_EQ(str_type->get_string(array_type->get_element_ptr(2, data)), "asdfg");
	array_type->dispose(data);
	delete[] data;
}

TEST(Dom, ValueStructs) {
	auto dom = pin<dom::Dom>::make();
	std::vector<pin<dom::FieldInfo>> fields{
			pin<dom::DomField>::make(dom->names()->get("name"), dom->mk_type(Kind::STRING)),
			pin<dom::DomField>::make(dom->names()->get("age"), dom->mk_type(Kind::FLOAT, 8))};
	auto struct_type = dom->mk_struct_type(
			dom->names()->get("andreyka")->get("test")->get("Person"),
			fields);
	auto name_field = fields[0];
	auto age_field = fields[1];
	auto data = new char[struct_type->get_size()];
	struct_type->init(data);
	EXPECT_EQ(name_field->type->get_string(name_field->get_data(data)), "");
	EXPECT_EQ(age_field->type->get_float(age_field->get_data(data)), 0);
	name_field->type->set_string("Andreyka", name_field->get_data(data));
	age_field->type->set_float(47.5, age_field->get_data(data));
	EXPECT_EQ(name_field->type->get_string(name_field->get_data(data)), "Andreyka");
	EXPECT_EQ(age_field->type->get_float(age_field->get_data(data)), 47.5);
	struct_type->dispose(data);
	delete[] data;
}

TEST(Dom, References) {
	auto dom = pin<Dom>::make();
	vector<pin<FieldInfo>> fields{
		pin<DomField>::make(dom->names()->get("data"), dom->mk_type(Kind::INT, 8)),
		pin<DomField>::make(dom->names()->get("next"), dom->mk_type(Kind::OWN)),
		pin<DomField>::make(dom->names()->get("prev"), dom->mk_type(Kind::WEAK))};
	auto item_type = dom->mk_class_type(
			dom->names()->get("andreyka")->get("test")->get("Item"),
			fields);
	auto data_field = fields[0];
	auto next_field = fields[1];
	auto prev_field = fields[2];
	own<DomItem> item = item_type->create_instance();
	auto data = Dom::get_data(item);
	data_field->type->set_int(5, data_field->get_data(data));
	EXPECT_EQ(data_field->type->get_int(data_field->get_data(data)), 5);
	EXPECT_TRUE(next_field->type->get_ptr(next_field->get_data(data)) == nullptr);
	EXPECT_TRUE(prev_field->type->get_ptr(prev_field->get_data(data)) == nullptr);
	auto n1 = item_type->create_instance();
	auto d1 = Dom::get_data(n1);
	prev_field->type->set_ptr(n1, prev_field->get_data(data));
	next_field->type->set_ptr(n1, next_field->get_data(data));
	prev_field->type->set_ptr(item, prev_field->get_data(d1));
	EXPECT_TRUE(prev_field->type->get_ptr(prev_field->get_data(data)) == n1);
	EXPECT_TRUE(next_field->type->get_ptr(next_field->get_data(data)) == n1);
	EXPECT_TRUE(prev_field->type->get_ptr(prev_field->get_data(d1)) == item);
	own<DomItem> copy = item;
	EXPECT_TRUE(copy != data);
	auto n2 = next_field->type->get_ptr(next_field->get_data(Dom::get_data(copy)));
	EXPECT_TRUE(n1 != n2);
	EXPECT_TRUE(prev_field->type->get_ptr(prev_field->get_data(Dom::get_data(copy))) == n2);
	EXPECT_TRUE(prev_field->type->get_ptr(prev_field->get_data(Dom::get_data(n2))) == copy);
}

TEST(Dom, DoubledStruct) {
	auto dom = pin<Dom>::make();
	vector<pin<FieldInfo>> fields{
		pin<DomField>::make(dom->names()->get("data"), dom->mk_type(Kind::INT, 8)),
		pin<DomField>::make(dom->names()->get("name"), dom->mk_type(Kind::STRING)),
		pin<DomField>::make(dom->names()->get("aaa"), dom->mk_type(Kind::UINT, 1))};
	auto struct_type = dom->mk_struct_type(dom->names()->get("Item"), fields);
	vector<pin<FieldInfo>> fields2{
		pin<DomField>::make(dom->names()->get("name"), dom->mk_type(Kind::STRING)),
		pin<DomField>::make(dom->names()->get("bbb"), dom->mk_type(Kind::FLOAT, 4)),
		pin<DomField>::make(dom->names()->get("data"), dom->mk_type(Kind::INT, 8))};
	auto struct_type2 = dom->mk_struct_type(dom->names()->get("Item"), fields2);
	EXPECT_TRUE(struct_type == struct_type2);
	EXPECT_TRUE(fields2[0] == fields[1]);
	EXPECT_TRUE(fields2[1] == FieldInfo::empty);
	EXPECT_TRUE(fields2[2] == fields[0]);
}

TEST(Dom, Sealed) {
	auto dom = pin<Dom>::make();
	auto int_type = dom->mk_type(Kind::INT, 4);
	auto ubyte_type = dom->mk_type(Kind::UINT, 1);
	auto array_type = dom->mk_type(Kind::VAR_ARRAY, 0, int_type);
	vector<pin<FieldInfo>> fields{
		pin<DomField>::make(dom->names()->get("x"), int_type),
		pin<DomField>::make(dom->names()->get("y"), int_type)};
	auto struct_type = dom->mk_struct_type(dom->names()->get("Item"), fields);
	dom->sealed = true;
	auto array_type2 = dom->mk_type(Kind::VAR_ARRAY, 0, int_type);
	EXPECT_TRUE(array_type == array_type2);
	auto array_type3 = dom->mk_type(Kind::VAR_ARRAY, 0, ubyte_type);
	EXPECT_TRUE(array_type3 == TypeInfo::empty);
	auto array_type4 = dom->mk_type(Kind::FIX_ARRAY, 3, int_type);
	EXPECT_TRUE(array_type4 == TypeInfo::empty);
	vector<pin<FieldInfo>> fields2{
		pin<DomField>::make(dom->names()->get("x"), int_type),
		pin<DomField>::make(dom->names()->get("y"), int_type)};
	auto struct_type2 = dom->mk_struct_type(dom->names()->get("Another"), fields2);
	EXPECT_TRUE(struct_type2 == TypeInfo::empty);
	EXPECT_TRUE(fields2[0] == FieldInfo::empty);
	EXPECT_TRUE(fields2[1] == FieldInfo::empty);
}

struct Point {
	int x, y;
	Point(int x, int y) : x(x), y(y) {}
	Point() : x(), y() {}
};

class Polygon : public DomItem {
public:
	Polygon() {}
	Point points[3];
	vector<Point> vp;
	DECLARE_DOM_CLASS(Polygon);
};

own<dom::TypeWithFills> Polygon::dom_type_;

TEST(Dom, Cpp) {
	auto dom = pin<Dom>::make();
    auto int_type = dom->mk_type(Kind::INT, sizeof(int));
	pin<dom::TypeWithFills> point_type = new dom::CppStructType<Point>(dom, {"ak", "Point" });
	point_type
		->field("x", pin<CppField<Point, int, &Point::x>>::make(int_type))
		->field("y", pin<CppField<Point, int, &Point::y>>::make(int_type));
	Polygon::dom_type_ = new dom::CppClassType<Polygon>(dom, {"ak", "Polygon"});
	Polygon::dom_type_
		->field("points", pin<CppField<Polygon, Point[3], &Polygon::points>>::make(
            dom->mk_type(Kind::FIX_ARRAY, 3, point_type)))
		->field("vp", pin<CppField<Polygon, vector<Point>, &Polygon::vp>>::make(
            new dom::VectorType<Point>(point_type)));
	auto root = pin<Polygon>::make();
	int num = 0;
	for (auto& p : root->points)
		p.x = (p.y = ++num) * 10;
	while (++num < 6)
		root->vp.emplace_back(num * 100, num);
	char* root_data = Dom::get_data(root);
	EXPECT_TRUE(Dom::get_type(root) == Polygon::dom_type_);
	auto polygon_points = Polygon::dom_type_->get_field(dom->names()->get("points"));
	auto polygon_vp = Polygon::dom_type_->get_field(dom->names()->get("vp"));
	auto point_x = point_type->get_field(dom->names()->get("x"));
	auto point_y = point_type->get_field(dom->names()->get("y"));
	char* points_data = polygon_points->get_data(root_data);
	EXPECT_TRUE(polygon_points->type->get_element_type() == point_type);
	EXPECT_EQ(polygon_points->type->get_elements_count(points_data), 3);
	for (int i = 0; i < 3; i++) {
		char* point_data = polygon_points->type->get_element_ptr(i, points_data);
		EXPECT_EQ(point_y->type->get_int(point_y->get_data(point_data)), i + 1);
		EXPECT_EQ(point_x->type->get_int(point_x->get_data(point_data)), (i + 1) * 10);
	}
	char* vp_data = polygon_vp->get_data(root_data);
	EXPECT_TRUE(polygon_vp->type->get_element_type() == point_type);
	EXPECT_EQ(polygon_vp->type->get_elements_count(vp_data), 2);
	for (int i = 0; i < 2; i++) {
		char* point_data = polygon_vp->type->get_element_ptr(i, vp_data);
		EXPECT_EQ(point_y->type->get_int(point_y->get_data(point_data)), i + 4);
		EXPECT_EQ(point_x->type->get_int(point_x->get_data(point_data)), (i + 4) * 100);
	}
}

}  // namespace
