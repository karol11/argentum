#ifndef _AK_DOM_H_
#define _AK_DOM_H_

#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include "ltm/ltm.h"

namespace dom {

using std::initializer_list;
using std::cerr;
using std::endl;
using std::string;
using std::vector;
using std::move;
using std::min;
using ltm::Object;
using ltm::own;
using ltm::weak;
using ltm::pin;
using std::unordered_map;
using std::function;
class TypeInfo;
class FieldInfo;
class DomItem;
class DomField;
class Dom;

class Name : public Object {
	friend class Dom;
public:
	pin<Name> peek(const string& name);
	pin<Name> get(const string& name);

	const weak<Name> domain;
	const string name;
protected:
	unordered_map<string, own<Name>> sub;

	Name(pin<Name> domain, string name)
	: domain(domain), name(name) { make_shared(); }
	LTM_COPYABLE(Name)
};

enum class Kind {
	EMPTY, INT, UINT, FLOAT, BOOL, STRING, OWN, WEAK, VAR_ARRAY, ATOM, FIX_ARRAY, STRUCT, CLASS, MAP, SET
};

class TypeInfo : public Object {
public:
	// all
	TypeInfo() { make_shared(); }
	virtual Kind get_kind() =0;
	// all but class
	virtual size_t get_size() { return 0; }
	virtual void init(char*) { report_error("unsupported init"); }
	virtual void dispose(char*) { report_error("unsupported dispose"); }
	virtual void move(char* src, char* dst) { report_error("unsupported move"); }
	virtual void copy(char* src, char* dst) { report_error("unsupported copy"); }

	// array
	virtual pin<TypeInfo> get_element_type(){ report_error("unsupported get_element_type"); return empty; }
	virtual char* get_element_ptr(size_t index, char*){ report_error("unsupported get_element"); return nullptr; }
	virtual size_t get_elements_count(char*){ report_error("unsupported get_elements_count"); return 0; }
	virtual void set_elements_count(size_t count, char*){ report_error("unsupported set_elements_count"); }
	// struct and class
	virtual void for_fields(function<void(pin<FieldInfo>)>){ report_error("unsupported for_fields"); }
	virtual size_t get_fields_count(){ report_error("unsupported get_fields_count"); return 0; }
	virtual pin<FieldInfo> get_field(pin<Name>);
	// class
	virtual pin<Name> get_name() { report_error("unsupported get_name"); return nullptr; }
	virtual pin<DomItem> create_instance() { report_error("unsupported create_instance"); return nullptr; }

	// maps / sets ignore keys
	// +get_element_type
	virtual pin<TypeInfo> get_key_type() { report_error("unsupported get_key_type"); return empty; } // only maps
	virtual char* get_element_ptr_by_key(char* key, char*) { report_error("unsupported get_element_ptr_by_key"); return nullptr; }  // key is a key-val
	virtual void delete_element_by_key(char* key, char*) { report_error("unsupported delete_element_by_key"); }
	virtual void add_element_by_key(char* key, char* value, char*) { report_error("unsupported add_element_by_key"); }  // key is ignored
	virtual void iterate_elements(char*, std::function<void(char* key, char* value)>) { report_error("unsupported iterate_elements"); } // key is nullptr

	// primitives
	virtual int64_t get_int(char*){ report_error("unsupported get_int"); return 0; }
	virtual void set_int(int64_t v, char*){ report_error("unsupported set_int"); }
	virtual uint64_t get_uint(char*){ report_error("unsupported get_uint"); return 0; }
	virtual void set_uint(uint64_t v, char*){ report_error("unsupported set_uint"); }
	virtual double get_float(char*){ report_error("unsupported get_float"); return 0; }
	virtual void set_float(double v, char*){ report_error("unsupported set_float"); }
	virtual bool get_bool(char*){ report_error("unsupported get_bool"); return false; }
	virtual void set_bool(bool v, char*){ report_error("unsupported set_bool"); }
	virtual pin<DomItem> get_ptr(char*){ report_error("unsupported get_ptr"); return nullptr; }
	virtual void set_ptr(const pin<DomItem>& v, char*){ report_error("unsupported set_ptr"); }
	virtual string get_string(char*){ report_error("unsupported get_str"); return ""; }
	virtual void set_string(string v, char*){ report_error("unsupported set_str"); }
	virtual pin<Name> get_atom(char*) { report_error("unsupported set_atom"); return nullptr; }
	virtual void set_atom(pin<Name>, char*) { report_error("unsupported set_atom"); }

	static void report_error(string message) { cerr << message << endl; }
	static const own<TypeInfo> empty;
};

class FieldInfo : public Object {
	friend class TypeWithFields;
public:
	FieldInfo(pin<Name> name, pin<TypeInfo> type) : name(name), type(type) {}
	virtual char* get_data(char* struct_ptr) =0;
	pin<Name> get_name() { return name; }
	const own<TypeInfo> type;
	static const own<FieldInfo> empty;
protected:
	own<Name> name;
	virtual void set_offset(ptrdiff_t offset) =0;
};

class DomField : public FieldInfo {
public:
	DomField(pin<Name> name, pin<TypeInfo> type) : FieldInfo(name, type) {}
	char* get_data(char* struct_ptr) override { return struct_ptr + offset; }
protected:
	void set_offset(ptrdiff_t offset) override { this->offset = offset; }
	ptrdiff_t offset = 0;
	LTM_COPYABLE(DomField)
};

class CppFieldBase : public FieldInfo {
	friend class TypeWithFills;
public:
	CppFieldBase(pin<TypeInfo> type) : FieldInfo(nullptr, type) { make_shared(); }
	void copy_to(Object*& d) override { d = nullptr; }
protected:
	void set_name(pin<Name> name) { this->name = name; }
	void set_offset(ptrdiff_t offset) override { TypeInfo::report_error("set offset not supported"); }
};

template<typename Dummy> struct FieldHelper {};
template<typename Base, typename MemberT>
struct FieldHelper<MemberT Base::*> {
	using base = Base;
	using member_t = MemberT;
};

template<auto field>
class CField : public CppFieldBase {
	friend class TypeWithFills;
public:
	CField(pin<TypeInfo> type) : CppFieldBase(type) {}
	virtual char* get_data(char* struct_ptr) { return reinterpret_cast<char*>(&(reinterpret_cast<typename FieldHelper<field>::base *>(struct_ptr)->*field)); }
};

template<typename STRUCT, typename MEMBER_T, MEMBER_T STRUCT::*field>
class CppField : public CppFieldBase
{
	friend class TypeWithFills;
public:
	CppField(pin<TypeInfo> type) : CppFieldBase(type) {}
	virtual char* get_data(char* struct_ptr){ return reinterpret_cast<char*>(&(reinterpret_cast<STRUCT*>(struct_ptr)->*field)); }
};

class TypeWithFields : public TypeInfo {
	friend class DomItemImpl;
public:
	pin<Name> get_name() override;
	void for_fields(function<void(pin<FieldInfo>)> action) override;
	size_t get_fields_count() override;
	pin<FieldInfo> get_field(pin<Name> name) override;

protected:
	// Builds layout (offsets and instance_size)
	TypeWithFields(pin<Name> name, vector<pin<FieldInfo>> init_fields);
	TypeWithFields() {}
	own<Name> name;
	unordered_map<own<Name>, own<FieldInfo>> fields;
	size_t instance_size = 0;
};

class TypeWithFills : public TypeWithFields {
public:
	TypeWithFills(pin<Dom> dom, initializer_list<const char*> names);
	TypeWithFills* field(const char* name, pin<CppFieldBase> field);
	Kind get_kind() override { return Kind::CLASS; }

protected:
	weak<Dom> dom;
	LTM_COPYABLE(TypeWithFills);
};

template<typename T>
class CppClassType : public TypeWithFills {
public:
	CppClassType(pin<Dom> dom, initializer_list<const char*> name) : TypeWithFills(dom, name) {}
	pin<DomItem> create_instance() override { return new T(); }
};

template<typename T>
class CppStructType: public TypeWithFills {
public:
	CppStructType(pin<Dom> dom, initializer_list<const char*> name) : TypeWithFills(dom, name) {}

	Kind get_kind() override { return Kind::STRUCT; }
	size_t get_size() override { return sizeof(T); }

	void init(char* data) override { new(data) T(); }
	void dispose(char* data) override { reinterpret_cast<T*>(data)->T::~T(); }
	void move(char* src, char* dst) override { new(dst) T(std::move(*reinterpret_cast<T*>(src))); };
	void copy(char* src, char* dst) override { new(dst) T(*reinterpret_cast<T*>(src)); };
};

template<typename T>
class VectorType : public CppStructType<vector<T>> {
public:
	VectorType(pin<TypeInfo> element_type)
		: CppStructType<vector<T>>(nullptr, {}), element_type(element_type) {}

	Kind get_kind() override { return Kind::VAR_ARRAY; }

	pin<TypeInfo> get_element_type() override { return element_type; }
	size_t get_elements_count(char* data) override { return reinterpret_cast<vector<T>*>(data)->size(); }
	char* get_element_ptr(size_t index, char* data) override {
		return index < get_elements_count(data)
			? reinterpret_cast<char*>(&reinterpret_cast<vector<T>*>(data)->at(index))
			: nullptr;
	}
	void set_elements_count(size_t count, char* data) override {
		return reinterpret_cast<vector<T>*>(data)->resize(count);
	}
protected:
	own<TypeInfo> element_type;
};

template<typename T>
class UnorderedSetType : public CppStructType<std::unordered_set<T>> {
public:
	UnorderedSetType(pin<TypeInfo> element_type)
		: CppStructType<std::unordered_set<T>>(nullptr, {}), element_type(element_type) {}

	Kind get_kind() override { return Kind::SET; }

	pin<TypeInfo> get_element_type() override { return element_type; }
	size_t get_elements_count(char* data) override { return reinterpret_cast<std::unordered_set<T>*>(data)->size(); }
	char* get_element_ptr_by_key(char* key, char* data) override {
		auto& d = *reinterpret_cast<std::unordered_set<T>*>(data);
		auto it = d.find(*reinterpret_cast<T*>(key));
		return it != d.end()
			? (char*) &*it
			: nullptr;
	}
	void delete_element_by_key(char* key, char* data) override {
		reinterpret_cast<std::unordered_set<T>*>(data)->erase(*reinterpret_cast<T*>(key));
	}
	void add_element_by_key(char* key, char* value, char* data) override {
		reinterpret_cast<std::unordered_set<T>*>(data)->insert(std::move(*reinterpret_cast<T*>(key)));
	}
	void iterate_elements(char* data, std::function<void(char* key, char* value)> on_item) override {
		for (auto& i : *reinterpret_cast<std::unordered_set<T>*>(data))
			on_item(nullptr, (char*) &i);
	}
protected:
	own<TypeInfo> element_type;
};

template<typename K, typename V>
class UnorderedMapType : public CppStructType<std::unordered_map<K, V>> {
public:
	UnorderedMapType(pin<TypeInfo> key_type, pin<TypeInfo> element_type)
		: CppStructType<std::unordered_map<K, V>>(nullptr, {}), key_type(key_type), element_type(element_type) {}

	Kind get_kind() override { return Kind::MAP; }

	pin<TypeInfo> get_key_type() override { return key_type; }
	pin<TypeInfo> get_element_type() override { return element_type; }
	size_t get_elements_count(char* data) override { return reinterpret_cast<std::unordered_map<K, V>*>(data)->size(); }
	char* get_element_ptr_by_key(char* key, char* data) override {
		auto& d = *reinterpret_cast<std::unordered_map<K, V>*>(data);
		auto it = d.find(*reinterpret_cast<K*>(key));
		return it != d.end()
			? (char*) &it->second
			: nullptr;
	}
	void delete_element_by_key(char* key, char* data) override {
		reinterpret_cast<std::unordered_map<K, V>*>(data)->erase(*reinterpret_cast<K*>(key));
	}
	void add_element_by_key(char* key, char* value, char* data) override {
		reinterpret_cast<std::unordered_map<K, V>*>(data)->insert({
			std::move(*reinterpret_cast<K*>(key)),
			std::move(*reinterpret_cast<V*>(value)) });
	}
	void iterate_elements(char* data, std::function<void(char* key, char* value)> on_item) override {
		for (auto& i : *reinterpret_cast<std::unordered_map<K, V>*>(data))
			on_item((char*) &i.first, (char*) &i.second);
	}
protected:
	own<TypeInfo> key_type;
	own<TypeInfo> element_type;
};

class DomItem: public Object
{
	friend class Dom;
protected:
	virtual char* get_data() { return reinterpret_cast<char*>(this); }
public:
	virtual pin<TypeInfo> get_type() const = 0;
	virtual string get_annotation() { return ""; }
};

class Dom: public Object {
	friend class TypeWithFills;
public:
	Dom(pin<Dom> parent = nullptr);
	pin<Name> names() { return root_name; }

	// Always returns elements of the root (terminal parent) DOM
	pin<TypeInfo> mk_type(Kind kind, size_t size = 0, pin<TypeInfo> item = nullptr);

	// If contains type in the whole chain of parents, returns it
	// otherwise adds type to the nearest not sealed DOM
	// if no such DOM, error.
	pin<TypeInfo> mk_struct_type(pin<Name> name, vector<pin<FieldInfo>>& fields) { return mk_class_or_struct_type(false, name, fields); }
	pin<TypeInfo> mk_class_type(pin<Name> name, vector<pin<FieldInfo>>& fields) { return mk_class_or_struct_type(true, name, fields); }

	void set_name(pin<DomItem> item, pin<Name> name);
	pin<Name> get_name(pin<DomItem> p);
	pin<DomItem> get_named(const pin<Name>& name);

	static pin<TypeInfo> get_type(const pin<DomItem>& item) { return item ? item->get_type() : TypeInfo::empty.pinned(); }
	static char* get_data(const pin<DomItem>& item) { return item ? item->get_data() : nullptr; }

	// If not null, this DOM searches its types in the whole chain of parents.
	weak<Dom> parent;

	// If not sealed, this DOM can register new types.
	bool sealed = false;

protected:
	pin<TypeInfo> mk_class_or_struct_type(bool is_class, pin<Name> name, vector<pin<FieldInfo>>& fields);
	void reg_cpp_type(TypeWithFills* type);

	own<Name> root_name = new Name(nullptr, "");
	unordered_map<weak<DomItem>, own<Name>> object_names;
	unordered_map<own<Name>, weak<DomItem>> named_objects;
	own<TypeInfo> atom_type, bool_type, string_type, own_ptr_type, weak_ptr_type;
	own<TypeInfo> int8_type, int16_type, int32_type, int64_type;
	own<TypeInfo> uint8_type, uint16_type, uint32_type, uint64_type;
	own<TypeInfo> float32_type, float64_type;
	unordered_map<own<TypeInfo>, own<TypeInfo>> var_arrays;
	unordered_map<own<Name>, own<TypeInfo>> named_types;
	unordered_map<own<TypeInfo>, unordered_map<size_t, own<TypeInfo>>> fixed_arrays;
	LTM_COPYABLE(Dom)
};

template<typename T>
pin<T> strict_cast(pin<DomItem> item) {
	return Dom::get_type(item) == T::dom_type_ ? item.cast<T>() : nullptr;
}

template<typename T> bool isa(const DomItem& v) {
	return v.get_type() == T::dom_type_;
}


template<typename MAP, typename KEY, typename VAL = typename MAP::mapped_type::wrapped>
pin<VAL> peek(const MAP& dict, const KEY& name) {
	auto it = dict.find(name);
	return it == dict.end() ? nullptr : it->second.pinned();
}

} // namespace dom

#define DECLARE_DOM_CLASS(NAME) \
	pin<dom::TypeInfo> get_type() const override { return dom_type_; } \
	static own<dom::TypeWithFills> dom_type_; \
	LTM_COPYABLE(NAME)

#endif // _AK_DOM_H_
