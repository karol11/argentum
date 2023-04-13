#include <vector>

#include "utils/fake-gunit.h"
#include "dom/dom-to-string.h"

namespace {

using std::vector;
using ltm::pin;
using ltm::own;
using dom::Dom;
using dom::DomField;
using dom::FieldInfo;
using dom::Kind;

TEST(DomToString, Basic) {
	auto dom = pin<Dom>::make();
	std::vector<pin<FieldInfo>> fields{
		pin<DomField>::make(dom->names()->get("name"), dom->mk_type(Kind::STRING)),
		pin<DomField>::make(dom->names()->get("age"), dom->mk_type(Kind::FLOAT, 8))};
	auto type = dom->mk_class_type(
        dom->names()->get("andreyka")->get("test")->get("Person"),
        fields);
	auto name_field = fields[0];
	auto age_field = fields[1];
	auto root = type->create_instance();
    auto data = Dom::get_data(root);
	name_field->type->set_string("Andreyka", name_field->get_data(data));
	age_field->type->set_float(47.5, age_field->get_data(data));
    EXPECT_EQ(std::to_string(root, dom),
        "andreyka_test_Person\n"  // no raw strings here, we can't predict line endings \n\r vs \n
        "name \"Andreyka\"\n"
        "age 47.5\n");
}

TEST(DomToString, Xrefs) {
	auto dom = pin<Dom>::make();
	vector<pin<FieldInfo>> fields{
		pin<DomField>::make(dom->names()->get("data"), dom->mk_type(Kind::INT, 8)),
		pin<DomField>::make(dom->names()->get("next"), dom->mk_type(Kind::OWN)),
		pin<DomField>::make(dom->names()->get("prev"), dom->mk_type(Kind::WEAK))};
	auto item_type = dom->mk_class_type(dom->names()->get("Item"), fields);
	auto data_field = fields[0];
	auto next_field = fields[1];
	auto prev_field = fields[2];
	auto item = item_type->create_instance();
	auto data = Dom::get_data(item);
	data_field->type->set_int(5, data_field->get_data(data));  // item->data = 5
	auto n1 = item_type->create_instance();
	auto d1 = Dom::get_data(n1);
	prev_field->type->set_ptr(n1, prev_field->get_data(data));  // item->prev = n1
	next_field->type->set_ptr(n1, next_field->get_data(data));  // item->next = n1
	prev_field->type->set_ptr(item, prev_field->get_data(d1));  // n1->prev = item
    EXPECT_EQ(std::to_string(item, dom),
        "Item._1\n"
        "data 5\n"
        "next Item._2\n"
        "\tdata 0\n"
        "\tnext null\n"
        "\tprev *=_1\n"
        "prev *=_2\n");
}

struct MapSetTest : dom::DomItem {
	std::unordered_map<std::string, int> test_map;
	std::unordered_set<std::string> test_set;
	DECLARE_DOM_CLASS(MapSetTest);
};
own<dom::TypeWithFills> MapSetTest::dom_type_;

TEST(DomToString, MapSet) {
	auto dom = pin<Dom>::make();
	auto int_type = dom->mk_type(Kind::INT, sizeof(int));
	auto str_type = dom->mk_type(Kind::STRING);
	auto map_t = new dom::UnorderedMapType<std::string, int>(str_type, int_type);
	auto set_t = new dom::UnorderedSetType<std::string>(str_type);
	MapSetTest::dom_type_ = (new dom::CppClassType<MapSetTest>(dom, { "MapSetTest" }))
		->field("map", pin<dom::CField<&MapSetTest::test_map>>::make(map_t))
		->field("set", pin<dom::CField<&MapSetTest::test_set>>::make(set_t));
	auto item = own<MapSetTest>::make();
	item->test_map = { {"asdf", 1} };
	item->test_set = { "qwer" };
	EXPECT_EQ(std::to_string(item, dom),
		"MapSetTest\n"
		"map :\n"
		"\tkey \"asdf\"\n"
		"\tval 1\n"
		"set :\n"
		"\t\"qwer\"\n");
}

}  // namespace
