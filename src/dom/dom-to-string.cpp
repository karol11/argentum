#include "dom/dom-to-string.h"

#include <sstream>
#include <unordered_map>

using std::string;
using std::string_view;
using std::unordered_map;
using ltm::pin;
using dom::Dom;
using dom::DomItem;
using dom::FieldInfo;
using dom::TypeInfo;
using dom::Name;
using dom::Kind;

namespace {

enum class Role {
	ARRAY_ITEM,
	OBJECT_FIELD,
};

struct Formatter {
	Formatter(const pin<DomItem>& root, pin<Dom> dom, std::ostream& result)
		: dom(dom)
		, result(result)
	{
		scan_ptr(root);
		dump_ptr(root, 0, Role::ARRAY_ITEM);
	}

	static char hex(int v) {
		v &= 0xf;
		return v < 10 ? v + '0' : v - 10 + 'a';
	}

	void print_indent(size_t i) {
		for (++i; --i;)
			result << '\t';
	}

	// returns true if it's ended with object at current indent
	bool dump(char* data, pin<TypeInfo> type, size_t indent, Role role, string_view object_id = "") {
		switch (type->get_kind()) {
		case Kind::FLOAT: result << type->get_float(data) << "\n"; break;
		case Kind::UINT: result << type->get_uint(data) << "\n"; break;
		case Kind::INT: result << type->get_int(data) << "\n"; break;
		case Kind::BOOL: result << (type->get_bool(data) ? "+\n" : "-\n"); break;
		case Kind::ATOM:
			result << "." << type->get_atom(data) << "\n";
			break;
		case Kind::STRING:
			result << "\"";
			for(char c: type->get_string(data)) {
				if (c < ' ' || c & 0x80 || c == '\\' || c == '\"')
					result << '\\' << hex(c >> 8) << hex(c);
				else
					result << c;
			}
			result << "\"\n";
			break;
		case Kind::STRUCT:
		case Kind::CLASS:
			result << type->get_name();
			if (!object_id.empty())
				result << object_id;
			result << '\n';
			type->for_fields([&](pin<FieldInfo> field){
				print_indent(indent);
				result << field->get_name();
				result << ' ';
				dump(field->get_data(data), field->type, indent + 1, Role::OBJECT_FIELD);
			});
			return true;
		case Kind::FIX_ARRAY:
		case Kind::VAR_ARRAY:
			result << ':';
			if (type->get_kind() == Kind::FIX_ARRAY)
				result  << type->get_elements_count(data);
			result << '\n';
			if (role == Role::ARRAY_ITEM)
				indent++;
			for (size_t i = 0, left = type->get_elements_count(data) + 1; --left; ++i) {
				print_indent(indent);
				if (dump(type->get_element_ptr(i, data), type->get_element_type(), indent, Role::ARRAY_ITEM) && left > 1)
					result << '\n';
			}
			break;
		case Kind::WEAK:
			result << '*';
			return dump_ptr(type->get_ptr(data), indent, role);
		case Kind::OWN:
			return dump_ptr(type->get_ptr(data), indent, role);
		default:
			// TODO: integrate with logger
			std::cerr << "unsupported kind" << int(type->get_kind()) << std::endl;
			exit(2);
		}
		return false;
	}

	// returns true if it's ended with object at current indent
	bool dump_ptr(const pin<DomItem>& ptr, size_t indent, Role role) {
		if (!ptr) {
			result << "null\n";
			return false;
		}
		auto& flags = subtree[ptr];
		auto name = dom->get_name(ptr);
		if (flags & SAVED || (flags & HAS_OWN) == 0) {
			result << "=";
			if (name)
				result << name;
			else
				result << ((flags & SAVED) ? "_" : "?") << (flags >> NUMERATOR_OFFSET);
			result << '\n';
			return false;
		}
		flags |= SAVED;
		string id =
			name ? "." + std::to_string(name) :
			(flags & (HAS_MULTIPLE_OWNS || HAS_WEAK)) ? "._" + std::to_string(flags >> NUMERATOR_OFFSET) :
			string();
		string annotation = ptr->get_annotation();
		if (!annotation.empty())
			id += " ; " + annotation;		
		return dump(Dom::get_data(ptr), Dom::get_type(ptr), indent, role, id);
	}

	void scan(char* data, pin<TypeInfo> type) {
		switch (type->get_kind()) {
		case Kind::STRUCT:
		case Kind::CLASS:
			type->for_fields([&](pin<FieldInfo> field){
				scan(field->get_data(data), field->type);
			});
			break;
		case Kind::FIX_ARRAY:
		case Kind::VAR_ARRAY:
			for (size_t i = 0, size = type->get_elements_count(data); i < size; ++i) {
				scan(type->get_element_ptr(i, data), type->get_element_type());
			}
			break;
		case Kind::WEAK:
			if (auto ptr = type->get_ptr(data)) {
				auto& flags = subtree[ptr];
				flags |= (flags & HAS_WEAK) << 1 | HAS_WEAK;
			}
			break;
		case Kind::OWN:
			scan_ptr(type->get_ptr(data));
			break;
		default:
			break;
		}
	}

	void scan_ptr(const pin<DomItem>& ptr) {
		if (!ptr)
			return;
		auto& flags = subtree[ptr];
		if ((flags & HAS_OWN) == 0)
			flags |= HAS_OWN | (numerator += (1 << NUMERATOR_OFFSET));
		else
			flags |= HAS_MULTIPLE_OWNS | HAS_OWN;
		scan(Dom::get_data(ptr), Dom::get_type(ptr));
	}

	size_t numerator = 0;
	pin<Dom> dom;
	unordered_map<pin<DomItem>, size_t> subtree;
	std::ostream& result;
	static constexpr size_t
		HAS_WEAK = 1,
		HAS_MULTIPLE_WEAKS = 1 << 1,
		HAS_OWN = 1 << 2,
		HAS_MULTIPLE_OWNS = 1 << 3,
		SAVED = 1 << 4,
		NUMERATOR_OFFSET = 5;
};

} // namespace

namespace std {

ostream& operator<< (ostream& dst, const std::pair<pin<DomItem>, pin<Dom>>& v) {
	Formatter f(v.first, v.second, dst);
	return dst;
}

string to_string(pin<DomItem> root, pin<Dom> dom) {
	std::stringstream result;
	Formatter f(root, dom, result);
	return result.str();
}

ostream& operator<< (ostream& dst, const pin<Name>& name) {
	if (!name)
		return dst << "NULL";
	if (name->domain && name->domain->domain) {
		dst << name->domain << '_';
	}
	return dst << name->name;
}

string to_string(const pin<Name>& name) {
	return (std::stringstream() << name).str();
}

}  // namespace std
