#include "dom/dom.h"

namespace dom {

pin<FieldInfo> TypeInfo::get_field(pin<Name>) {
	report_error("unsupported get_field"); return FieldInfo::empty;
}

pin<Name> Name::peek(const string& name) {
	auto it = sub.find(name);
	return it == sub.end() ? nullptr : it->second;
}

pin<Name> Name::get(const string& name) {
	auto it = sub.find(name);
	return it == sub.end() || !it->second ? sub[name] = new Name(this, name) : it->second;
}

template<typename T, Kind ID>
class PrimitiveType : public TypeInfo
{
public:
	Kind get_kind() override { return ID; }
	void init(char* data) override { new(data) T(); }
	void dispose(char* data) { reinterpret_cast<T*>(data)->T::~T(); }
	size_t get_size() override { return sizeof(T); }
	void move(char* src, char* dst) override { new(dst) T(std::move(*reinterpret_cast<T*>(src))); };
	void copy(char* src, char* dst) override { new(dst) T(*reinterpret_cast<T*>(src)); };
};

class StringType : public PrimitiveType<string, Kind::STRING>
{
public:
	string get_string(char* data) override { return *reinterpret_cast<string*>(data); }
	void set_string(string v, char* data) override { *reinterpret_cast<string*>(data) = std::move(v); }
	void init(char* data) override { new(data) string; }
	LTM_COPYABLE(StringType)
};

template<typename T>
class IntType : public PrimitiveType<T, Kind::INT>
{
public:
	int64_t get_int(char* data) override { return *reinterpret_cast<T*>(data); }
	void set_int(int64_t v, char* data) override { *reinterpret_cast<T*>(data) = T(v); }
	LTM_COPYABLE(IntType)
};

template<typename T>
class UIntType : public PrimitiveType<T, Kind::UINT>
{
public:
	uint64_t get_uint(char* data) override { return *reinterpret_cast<T*>(data); }
	void set_uint(uint64_t v, char* data) override { *reinterpret_cast<T*>(data) =T(v); }
	LTM_COPYABLE(UIntType)
};

template<typename T>
class FloatType : public PrimitiveType<T, Kind::FLOAT>
{
public:
	double get_float(char* data) override { return *reinterpret_cast<T*>(data); }
	void set_float(double v, char* data) override { *reinterpret_cast<T*>(data) = T(v); }
	LTM_COPYABLE(FloatType)
};

class BoolType : public PrimitiveType<bool, Kind::BOOL>
{
public:
	bool get_bool(char* data) override { return *reinterpret_cast<bool*>(data); }
	void set_bool(bool v, char* data) override { *reinterpret_cast<bool*>(data) = v; }
	LTM_COPYABLE(BoolType)
};

template<typename PTR, Kind ID>
class PtrType : public PrimitiveType<PTR, ID>
{
public:
	pin<DomItem> get_ptr(char* data) override { return *reinterpret_cast<PTR*>(data); }
	void set_ptr(const pin<DomItem>& v, char* data) override { *reinterpret_cast<PTR*>(data) = v; }
	LTM_COPYABLE(PtrType)
};

class AtomType : public PrimitiveType<own<Name>, Kind::ATOM>
{
public:
	pin<Name> get_atom(char* data) override { return *reinterpret_cast<own<Name>*>(data); }
	void set_atom(pin<Name> v, char* data) override { *reinterpret_cast<own<Name>*>(data) = v; }
	LTM_COPYABLE(AtomType)
};

class VarArrayType : public TypeInfo
{
	struct Data{
		size_t count;
		char* items;
	};
public:
	VarArrayType(pin<TypeInfo> element_type)
		: element_type(element_type)
		, element_size(element_type->get_size()) {}

	size_t get_size() override { return sizeof(Data); }

	Kind get_kind() override { return Kind::VAR_ARRAY; }

	pin<TypeInfo> get_element_type() override { return element_type; }
	size_t get_elements_count(char* data) override { return reinterpret_cast<Data*>(data)->count; }

	void init(char* data) override {
		Data* d = reinterpret_cast<Data*>(data);
		d->count = 0;
		d->items = nullptr;
	}

	void dispose(char* data) override {
		Data* d = reinterpret_cast<Data*>(data);
		char* item = d->items;
		for (size_t i = d->count + 1; --i; item += element_size) {
			element_type->dispose(item);
		}
		delete[] d->items;
	};

	void move(char* src, char* dst) override {
		Data* d = reinterpret_cast<Data*>(dst);
		Data* s = reinterpret_cast<Data*>(src);
		Data t = *s;
		init(src);
		dispose(dst);
		*d = t;
	};

	void copy(char* src, char* dst) override {
		Data* d = reinterpret_cast<Data*>(dst);
		Data* s = reinterpret_cast<Data*>(src);
		Data t{s->count, new char[element_size * s->count]};
		char* si = s->items;
		char* di = t.items;
		for (size_t i = s->count + 1; --i; si += element_size, di += element_size) {
			element_type->copy(si, di);
		}
		dispose(dst);
		*d = t;    
	};

	char* get_element_ptr(size_t index, char* data) override {
		return index < reinterpret_cast<Data*>(data)->count
			? reinterpret_cast<Data*>(data)->items + index * element_size
			: nullptr;
	}

	void set_elements_count(size_t count, char* data) override {
		Data* v = reinterpret_cast<Data*>(data);
		if (v->count == count)
			return;
		char* dst = new char[count * element_size];
		char* si = v->items;
		char* di = dst;
		for (size_t i = min(v->count, count) + 1; --i; si += element_size, di += element_size) {
			element_type->move(si, di);
			element_type->dispose(si);
		}
		if (v->count < count) {
			for (size_t i = count - v->count + 1; --i; di += element_size) {
				element_type->init(di);
			}
		} else {
			for (size_t i = v->count - count + 1; --i; si += element_size) {
				element_type->dispose(si);
			}
		}
		delete[] v->items;
		v->items = dst;
		v->count = count;
	}

protected:
	own<TypeInfo> element_type;
	size_t element_size;
	LTM_COPYABLE(VarArrayType)
};

class FixArrayType : public TypeInfo
{
public:
	FixArrayType(pin<TypeInfo> element_type, size_t elements_count)
		: element_type(element_type)
		, elements_count(elements_count)
		, element_size(element_type->get_size()) {}

	size_t get_size() override { return element_size * elements_count; }

	Kind get_kind() override { return Kind::FIX_ARRAY; }

	pin<TypeInfo> get_element_type() override { return element_type; }

	char* get_element_ptr(size_t index, char* data) override {
		return index < elements_count ? data + element_size * index : nullptr;
	}

	size_t get_elements_count(char*) override { return elements_count; }

	void init(char* data) override {
		for (size_t i = elements_count + 1; --i; data += element_size) {
			element_type->init(data);
		}
	}

	void dispose(char* data) override {
		for (size_t i = elements_count + 1; --i; data += element_size) {
			element_type->dispose(data);
		}    
	};

	void move(char* src, char* dst) override {
		for (size_t i = elements_count + 1; --i; src += element_size, dst += element_size) {
			element_type->move(src, dst);
		}
	};

	void copy(char* src, char* dst) override {
		for (size_t i = elements_count + 1; --i; src += element_size, dst += element_size) {
			element_type->copy(src, dst);
		}
	};

protected:
	own<TypeInfo> element_type;
	size_t elements_count;
	size_t element_size;
	LTM_COPYABLE(FixArrayType)
};

class DomItemImpl : public DomItem
{
	friend class ClassType;

public:
	pin<TypeInfo> get_type() const override { return type; }
	char* get_data() override { return data; }

protected:
	DomItemImpl(pin<TypeWithFields> type) :type(move(type)) {}
	void copy_to(Object*& d) override {
		d = alloc(type);
	auto dst = static_cast<DomItemImpl*>(d)->data;
	for (auto& f : type->fields) {
		f.second->type->copy(f.second->get_data(data), f.second->get_data(dst));
	}
	}

	static DomItemImpl* alloc(const pin<TypeWithFields>& type) {
		return new (new char[sizeof(DomItemImpl) + type->instance_size]) DomItemImpl(type);
	}

	void internal_dispose() noexcept override {
		for (auto& f : type->fields){
			f.second->type->dispose(f.second->get_data(data));
		}
		this->DomItem::~DomItem();
		delete[] reinterpret_cast<char*>(this);
	}

	const own<TypeWithFields> type;
	char data[1];
};

TypeWithFields::TypeWithFields(pin<Name> name, vector<pin<FieldInfo>> init_fields)
	: name(name)
{
	instance_size = 0;
	for (auto& f : init_fields) {
		fields.insert({ f->name, f });
		f->set_offset(instance_size);
		instance_size += f->type->get_size();
	}
}

pin<Name> TypeWithFields::get_name() { return name; }

void TypeWithFields::for_fields(function<void(pin<FieldInfo>)> action) {
	for (const auto& f : fields)
		action(f.second);
}

size_t TypeWithFields::get_fields_count() {
	return fields.size();
}

pin<FieldInfo> TypeWithFields::get_field(pin<Name> name) {
	auto it = fields.find(name);
	return it == fields.end() ? FieldInfo::empty.pinned() : it->second.pinned();
}

TypeWithFills::TypeWithFills(pin<Dom> dom, initializer_list<const char*> names)
	: dom(dom)
{
	if (dom) {
		name = dom->names();
		for (auto n : names) {
			name = name->get(n);
		}
		dom->reg_cpp_type(this);
	}
}

TypeWithFills* TypeWithFills::field(const char* name, pin<CppFieldBase> field) {
	field->set_name(dom->names()->get(name));
	fields.insert({ field->get_name(), field });
	return this;
}

class ClassType : public TypeWithFields {
public:
	ClassType(pin<Name> name, vector<pin<FieldInfo>> init_fields)
		: TypeWithFields(name, init_fields) {}
	Kind get_kind() override { return Kind::CLASS; }
	pin<DomItem> create_instance() override {
		DomItemImpl* r = DomItemImpl::alloc(this);
		auto data = r->get_data();
		for (auto& f : fields) {
			f.second->type->init(f.second->get_data(data));
		}
		return r;
	}

protected:
	LTM_COPYABLE(ClassType)
};

class StructType : public TypeWithFields
{
public:
	StructType(pin<Name> name, vector<pin<FieldInfo>> init_fields)
		: TypeWithFields(name, init_fields) {}

	Kind get_kind() override { return Kind::STRUCT; }
	size_t get_size() override { return instance_size; }

	void init(char* data) override {
		for (auto& f : fields){
			f.second->type->init(f.second->get_data(data));
		}
	}

	void dispose(char* data) override {
		for (auto& f : fields){
			f.second->type->dispose(f.second->get_data(data));
		}
	}

	void move(char* src, char* dst) override {
		for (auto& f : fields) {
			f.second->type->move(f.second->get_data(src), f.second->get_data(dst));
		}
	};

	void copy(char* src, char* dst) override {
		for (auto& f : fields) {
			f.second->type->copy(f.second->get_data(src), f.second->get_data(dst));
		}
	};

protected:
	LTM_COPYABLE(StructType)
};

Dom::Dom(pin<Dom> parent) : parent(parent) {
	if (!parent) {
		atom_type = new AtomType;
		bool_type = new BoolType;
		string_type = new StringType;
		own_ptr_type = new PtrType<own<DomItem>, Kind::OWN>;
		weak_ptr_type = new PtrType<weak<DomItem>, Kind::WEAK>;
		int8_type = new IntType<int8_t>;
		int16_type = new IntType<int16_t>;
		int32_type = new IntType<int32_t>;
		int64_type = new IntType<int64_t>;
		uint8_type = new UIntType<uint8_t>;
		uint16_type = new UIntType<uint16_t>;
		uint32_type = new UIntType<uint32_t>;
		uint64_type = new UIntType<uint64_t>;
		float32_type = new FloatType<float>;
		float64_type = new FloatType<double>;
	}
}

pin<TypeInfo> Dom::mk_type(Kind kind, size_t size, pin<TypeInfo> item) {
	if (parent)
		return parent->mk_type(kind, size, move(item));
	switch(kind) {
	case Kind::ATOM: return atom_type;
	case Kind::BOOL: return bool_type;
	case Kind::STRING: return string_type;
	case Kind::WEAK: return weak_ptr_type;
	case Kind::OWN: return own_ptr_type;
	case Kind::FLOAT: return size <= 4 ? float32_type : float64_type;
	case Kind::INT:
		return
			size == 1 ? int8_type :
			size == 2 ? int16_type :
			size <= 4 ? int32_type :
			int64_type;
	case Kind::UINT:
		return
			size == 1 ? uint8_type :
			size == 2 ? uint16_type :
			size <= 4 ? uint32_type :
			uint64_type;
	case Kind::VAR_ARRAY: {
		if (sealed) {
			auto it = var_arrays.find(item);
			return it == var_arrays.end() ? TypeInfo::empty : it->second;
		}
		auto& result = var_arrays[item];
		if (!result)
			result = new VarArrayType(item);
		return result; }
	case Kind::FIX_ARRAY: {
		if (sealed) {
			auto it = fixed_arrays.find(item);
			if (it == fixed_arrays.end())
				return TypeInfo::empty;
			auto size_it = it->second.find(size);
			return size_it == it->second.end() ? TypeInfo::empty : size_it->second;
		}
		auto& result = fixed_arrays[item][size];
		if (!result) result = new FixArrayType(item, size);
		return result; }
	default:
		return nullptr;
	}
}

void Dom::set_name(pin<DomItem> item, pin<Name> name) {
	auto it = named_objects.find(name);
	if (it != named_objects.end())
		object_names.erase(it->second);
	object_names[item] = name;
	named_objects[name] = item;
}

pin<Name> Dom::get_name(pin<DomItem> p) {
	if (!p.has_weak())
		return nullptr;
	auto it = object_names.find(p);
	if (it == object_names.end())
		return nullptr;
	if (!it->first) {
		named_objects.erase(it->second);
		object_names.erase(it);
		return nullptr;
	}
	return it->second;
}

pin<DomItem> Dom::get_named(const pin<Name>& name) {
	auto it = named_objects.find(name);
	if (it == named_objects.end())
		return nullptr;
	if (!it->second) {
		object_names.erase(it->second);
		named_objects.erase(it);
		return nullptr;
	}
	return it->second;
}

pin<TypeInfo> Dom::mk_class_or_struct_type(bool is_class, pin<Name> name, vector<pin<FieldInfo>>& fields) {
	for (pin<Dom> d = this; d; d = d->parent) {
		if (auto it = d->named_types.find(name); it != d->named_types.end()) {
			for (auto& field : fields)
				field = it->second->get_field(field->get_name());
			return it->second;
		}
	}
	for (pin<Dom> d = this; d; d = d->parent) {
		if (!sealed) {
			auto& result = named_types[name];
			result = is_class
				? pin<TypeInfo>::make<ClassType>(name, fields)
				: pin<TypeInfo>::make<StructType>(name, fields);
			return result;
		}
	}
	for (auto& f : fields)
		f = FieldInfo::empty;
	return TypeInfo::empty;
}

void Dom::reg_cpp_type(TypeWithFills* type) {
	auto& result = named_types[type->get_name()];
	if (result) {
		cerr << "type " << std::to_string(type->get_name()) << " redefinition" << endl;
	} else {
		result = type;
	}
}

class EmptyTypeInfo : public TypeInfo
{
public:
	virtual Kind get_kind() { return Kind::EMPTY; }
	virtual size_t get_size() { return 0;}
	virtual void init(char*){}
	virtual void move(char* src, char* dst) {}
	virtual void copy(char* src, char* dst) {}
	virtual pin<TypeInfo> get_element_type(){ return this; }
	virtual pin<TypeInfo> get_ptr_type(char*){ return this; }
	virtual void report_error(string message) {}
	LTM_COPYABLE(EmptyTypeInfo);
};

const own<TypeInfo> TypeInfo::empty = new EmptyTypeInfo;
const own<FieldInfo> FieldInfo::empty = new DomField(nullptr, TypeInfo::empty);

} // namespace std
