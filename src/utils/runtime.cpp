#include "utils/runtime.h"

#include <vector>
#include <cstring>

#include "compiler/ast.h"
#include "utils/utf8.h"

using std::vector;
using std::pair;
using std::memset;
using std::memcpy;
using std::memmove;

namespace runtime {

	int leak_detector_counter = 0;
	int current_allocated = 0;
	int max_allocated = 0;


	void* rt_alloc(size_t size) {
		leak_detector_counter++;
		if ((current_allocated += size) > max_allocated)
			max_allocated = current_allocated;
		auto r = (size_t*)(new char[size + sizeof(size_t)]);
		*r = size;
		return r + 1;
	}
	void rt_free(void* data) {
		leak_detector_counter--;
		auto r = (size_t*) data;
		current_allocated -= r[-1];
		delete[] (char*)(r - 1);
	}
	bool leak_detector_ok() {
		std::cout << "Max mem:" << max_allocated << std::endl;
		return leak_detector_counter == 0;
	}

	struct Object* copy_head = nullptr;
	vector<pair<Object*, void (*)(Object*)>> copy_fixers; // Used only for objects with manual afterCopy operators.

	void Object::release(Object* obj) {
		if (!obj || size_t(obj) < 256)
			return;
		if ((obj->counter & CTR_WEAKLESS) != 0) {
			if ((obj->counter -= CTR_STEP) >= CTR_STEP)
				return;
		} else {
			auto wb = reinterpret_cast<Weak*>(obj->counter);
			if ((wb->org_counter -= CTR_STEP) >= CTR_STEP)
				return;
			wb->target = nullptr;
			obj->counter = 0;
			release_weak(wb);
		}
		reinterpret_cast<const Vmt*>(obj->dispatcher)[-1].dispose(obj);
		rt_free(obj);
	}

	Object* Object::retain(Object* obj) {
		if (obj && size_t(obj) >= 256) {
			if ((obj->counter & CTR_WEAKLESS) != 0) {
				obj->counter += CTR_STEP;
			} else {
				reinterpret_cast<Weak*>(obj->counter)->org_counter += CTR_STEP;
			}
		}
		return obj;
	}

	void* Object::allocate(size_t size) {
		auto r = rt_alloc(size);
		memset(r, 0, size);
		reinterpret_cast<Object*>(r)->counter = CTR_STEP | CTR_WEAKLESS;
		return r;
	}

	Object* Object::copy(Object* src) {
		Object* dst = copy_object_field(src);
		Object* c = nullptr;
		Weak* wb = nullptr;
		for (Object* i = copy_head; i;) {
			switch (get_ptr_tag(i)) {
			case TG_OBJECT:
				if (c)
					c->counter = CTR_STEP | CTR_WEAKLESS;
				c = untag_ptr<Object*>(i);
				i = reinterpret_cast<Object*>(c->counter);
				break;
			case TG_WEAK_BLOCK:
				wb = untag_ptr<Weak*>(i);
				i = wb->target;
				wb->target = c;
				c = nullptr;
				break;
			case TG_WEAK:
			{
				Weak** w = untag_ptr<Weak**>(i);
				i = reinterpret_cast<Object*>(*w);
				*w = wb;
				wb->wb_counter++;
			}
			break;
			}
		}
		if (c)
			c->counter = CTR_STEP | CTR_WEAKLESS;
		copy_head = nullptr;
		return dst;
	}

	Object* Object::copy_object_field(Object* src) {
		if (!src || size_t(src) < 256)
			return src;
		if ((src->counter & CTR_WEAKLESS
			? src->counter
			: reinterpret_cast<Weak*>(src->counter)->org_counter)
			& CTR_FROZEN) {
			return retain(src);
		}
		const auto& vmt = reinterpret_cast<const Vmt*>(src->dispatcher)[-1];
		auto d = reinterpret_cast<Object*>(rt_alloc(vmt.instance_alloc_size));
		memcpy(d, src, vmt.instance_alloc_size);
		reinterpret_cast<Object*>(d)->counter = CTR_STEP | CTR_WEAKLESS;
		vmt.copy_ref_fields(d, src);
		if ((src->counter & CTR_WEAKLESS) == 0) { // has weak block
			auto wb = reinterpret_cast<Weak*>(src->counter);
			if (wb->target == src) { // no weak copied yet
				wb->target = tag_ptr<Object*>(d, TG_OBJECT);
				d->counter = reinterpret_cast<uintptr_t>(copy_head);
				copy_head = tag_ptr<Object*>(src, TG_OBJECT);
			} else {
				auto dst_wb = reinterpret_cast<Weak*>(rt_alloc(sizeof(Weak)));
				d->counter = reinterpret_cast<uintptr_t>(dst_wb);
				dst_wb->org_counter = CTR_STEP | CTR_WEAKLESS;
				void* i = reinterpret_cast<Weak*>(src->counter)->target;
				uintptr_t dst_wb_locks = 1;
				while (get_ptr_tag(i) == TG_WEAK) {
					Weak** w = untag_ptr<Weak**>(i);
					i = *w;
					*w = dst_wb;
					dst_wb_locks++;
				}
				dst_wb->wb_counter = dst_wb_locks;
				dst_wb->target = reinterpret_cast<Object*>(i);
				wb->target = tag_ptr<Object*>(d, TG_OBJECT);
			}
		}
		while (!copy_fixers.empty()) {  // TODO retain objects in copy_fixers vector.
			copy_fixers.back().second(copy_fixers.back().first);
			copy_fixers.pop_back();
		}
		return reinterpret_cast<Object*>(d);
	}

	void Object::make_shared(Object* obj) {  // TODO: implement hierarchy freeze
		if ((obj->counter & CTR_WEAKLESS) != 0) {
			obj->counter |= CTR_FROZEN;
		} else {
			auto wb = reinterpret_cast<Weak*>(obj->counter);
			wb->org_counter |= CTR_FROZEN;
		}
	}

	Object::Weak* Object::retain_weak(Object::Weak* w) {
		if (w && size_t(w) >= 256)
			++w->wb_counter;
		return w;
	}

	void Object::release_weak(Object::Weak* w) {
		if (!w || size_t(w) < 256)
			return;
		if (--w->wb_counter != 0)
			return;
		rt_free(w);
	}

	void Object::copy_weak_field(void** dst, Object::Weak* src) {
		if (!src || size_t(src) < 256) {
			*dst = src;
		} else if (!src->target) {
			src->wb_counter++;
			*dst = src;
		} else {
			switch (get_ptr_tag(src->target)) {
			case TG_WEAK_BLOCK: // tagWB == 0, so it is an uncopied object
				*dst = copy_head;
				copy_head = tag_ptr<Object*>(src->target, TG_OBJECT);
				src->target = tag_ptr<Object*>(dst, TG_WEAK);
				break;
			case TG_WEAK: // already accessed by weak in this copy
				*dst = src->target;
				src->target = tag_ptr<Object*>(dst, TG_WEAK);
				break;
			case TG_OBJECT:
			{ // already copied
				Object* copy = untag_ptr<Object*>(src->target);
				auto cwb = reinterpret_cast<Weak*>(copy->counter);
				if (!cwb || get_ptr_tag(cwb) == TG_OBJECT) // has no wb yet
				{
					cwb = reinterpret_cast<Weak*>(rt_alloc(sizeof(Weak)));
					cwb->org_counter = CTR_STEP;
					cwb->wb_counter = CTR_STEP;
					cwb->target = reinterpret_cast<Object*>(copy->counter);
					copy->counter = reinterpret_cast<uintptr_t>(tag_ptr<void*>(cwb, TG_WEAK_BLOCK));  // workaround for in C++ compiler error
				} else
					cwb = untag_ptr<Weak*>(cwb);
				cwb->wb_counter++;
				*dst = cwb;
			} break;
			}
		}
	}

	Object::Weak* Object::mk_weak(Object* obj) { // obj can't be null
		if (obj->counter & CTR_WEAKLESS) {
			auto w = reinterpret_cast<Weak*>(rt_alloc(sizeof(Weak)));
			w->org_counter = obj->counter;
			w->target = obj;
			w->wb_counter = 2; // one from obj and one from `mk_weak` result
			obj->counter = reinterpret_cast<std::uintptr_t>(w);
			return w;
		}
		auto w = reinterpret_cast<Weak*>(obj->counter);
		w->wb_counter++;
		return w;
	}
	Object* Object::deref_weak(Object::Weak* w) {
		if (!w || size_t(w) < 256 || !w->target) {
			return nullptr;
		}
		w->org_counter += CTR_STEP;
		return w->target;
	}
	void Object::reg_copy_fixer(Object* object, void (*fixer)(Object*)) {
		copy_fixers.push_back({ object, fixer });
	}

	int32_t StringObj::get_char(StringObj* s) {
		return s->ptr && *s->ptr
			? get_utf8(s->ptr)
			: 0;
	}
	void StringObj::copy_fields(void* dst, void* src) {
		auto d = reinterpret_cast<StringObj*>(dst);
		auto s = reinterpret_cast<StringObj*>(src);
		d->ptr = s->ptr;
		d->buffer = s->buffer;
		if (d->buffer)
			d->buffer->counter++;
	}
	void StringObj::dispose_fields(void* str) {
		auto s = reinterpret_cast<StringObj*>(str);
		if (s->buffer && --s->buffer->counter == 0)
			delete s->buffer;
	}

	int64_t Blob::get_size(Blob* b) {
		return b->size;
	}
	void Blob::insert_items(Blob* b, uint64_t index, uint64_t count) {
		if (!count || index > b->size)
			return;
		auto new_data = new int64_t[b->size + count];
		memcpy(new_data, b->data, sizeof(int64_t) * index);
		memset(new_data + index, 0, sizeof(int64_t) * count);
		memcpy(new_data + index + count, b->data + index, sizeof(int64_t) * (b->size - index));
		delete[] b->data;
		b->data = new_data;
		b->size += count;
	}
	void Blob::delete_blob_items(Blob* b, uint64_t index, uint64_t count) {
		if (!count || index > b->size || index + count > b->size)
			return;
		auto new_data = new int64_t[b->size - count];
		memcpy(new_data, b->data, sizeof(int64_t) * index);
		memcpy(new_data + index, b->data + index + count, sizeof(int64_t) * (b->size - index));
		delete[] b->data;
		b->data = new_data;
		b->size -= count;
	}
	void Blob::delete_array_items(Blob* b, uint64_t index, uint64_t count) {
		if (!count || index > b->size || index + count > b->size)
			return;
		auto data = reinterpret_cast<Object**>(b->data) + index;
		for (uint64_t i = count; i != 0; i--, data++) {
			Object::release(*data);
			*data = nullptr;
		}
		delete_blob_items(b, index, count);
	}
	void Blob::delete_weak_array_items(Blob* b, uint64_t index, uint64_t count) {
		if (!count || index > b->size || index + count > b->size)
			return;
		auto data = reinterpret_cast<Object::Weak**>(b->data) + index;
		for (uint64_t i = count; i != 0; i--, data++) {
			Object::release_weak(*data);
			*data = nullptr;
		}
		delete_blob_items(b, index, count);
	}
	bool Blob::move_array_items(Blob* blob, uint64_t a, uint64_t b, uint64_t c) {
		if (a >= b || b >= c || c > blob->size)
			return false;
		auto temp = new uint64_t[b - a];
		memmove(temp, blob->data + a, sizeof(uint64_t) * (b - a));
		memmove(blob->data + a, blob->data + b, sizeof(uint64_t) * (c - b));
		memmove(blob->data + a + (c - b), temp, sizeof(uint64_t) * (b - a));
		delete[] temp;
		return true;
	}

	int64_t Blob::get_at(Blob* b, uint64_t index) {
		return index < b->size ? b->data[index] : 0;
	}
	void Blob::set_at(Blob* b, uint64_t index, int64_t val) {
		if (index < b->size)
			b->data[index] = val;
	}
	int64_t Blob::get_i8_at(Blob* b, uint64_t index) {
		return index / sizeof(int64_t) < b->size
			? reinterpret_cast<uint8_t*>(b->data)[index]
			: 0;
	}
	void Blob::set_i8_at(Blob* b, uint64_t index, int64_t val) {
		if (index / sizeof(int64_t) < b->size)
			reinterpret_cast<uint8_t*>(b->data)[index] = static_cast<uint8_t>(val);
	}
	int64_t Blob::get_i16_at(Blob* b, uint64_t index) {
		return index / sizeof(int64_t) * sizeof(int16_t) < b->size
			? reinterpret_cast<uint16_t*>(b->data)[index]
			: 0;
	}
	void Blob::set_i16_at(Blob* b, uint64_t index, int64_t val) {
		if (index / sizeof(int64_t) * sizeof(int16_t) < b->size)
			reinterpret_cast<uint16_t*>(b->data)[index] = static_cast<uint16_t>(val);
	}
	int64_t Blob::get_i32_at(Blob* b, uint64_t index) {
		return index / sizeof(int64_t) * sizeof(int32_t) < b->size
			? reinterpret_cast<uint32_t*>(b->data)[index]
			: 0;
	}
	void Blob::set_i32_at(Blob* b, uint64_t index, int64_t val) {
		if (index / sizeof(int64_t) * sizeof(int32_t) < b->size)
			reinterpret_cast<uint32_t*>(b->data)[index] = static_cast<uint32_t>(val);
	}
	bool Blob::blob_copy(Blob* dst, uint64_t dst_index, Blob* src, uint64_t src_index, uint64_t bytes) {
		if ((src_index + bytes) / sizeof(int64_t) >= src->size || (dst_index + bytes) / sizeof(int64_t) >= dst->size)
			return false;
		memmove(reinterpret_cast<uint8_t*>(dst->data) + dst_index, reinterpret_cast<uint8_t*>(src->data) + src_index, bytes);
		return true;
	}

	Object* Blob::get_ref_at(Blob* b, uint64_t index) {
		return index < b->size
			? Object::retain(reinterpret_cast<Object*>(b->data[index]))
			: nullptr;
	}
	Object::Weak* Blob::get_weak_at(Blob* b, uint64_t index) {
		return index < b->size
			? Object::retain_weak(reinterpret_cast<Object::Weak*>(b->data[index]))
			: nullptr;
	}
	void Blob::set_own_at(Blob* b, uint64_t index, Object* val) {
		if (index < b->size) {
			auto dst = reinterpret_cast<Object**>(b->data) + index;
			Object::retain(val);
			Object::release(*dst);
			*dst = val;
		}
	}
	void Blob::set_weak_at(Blob* b, uint64_t index, Object::Weak* val) {
		if (index < b->size) {
			auto dst = reinterpret_cast<Object::Weak**>(b->data) + index;
			val = Object::retain_weak(val);
			Object::release_weak(*dst);
			*dst = val;
		}
	}
	void Blob::copy_container_fields(void* dst, void* src) {
		auto d = reinterpret_cast<Blob*>(dst);
		auto s = reinterpret_cast<Blob*>(src);
		d->size = s->size;
		d->data = new int64_t[d->size];
		memcpy(d->data, s->data, sizeof(int64_t) * d->size);
	}
	void Blob::copy_array_fields(void* dst, void* src) {
		auto d = reinterpret_cast<Blob*>(dst);
		auto s = reinterpret_cast<Blob*>(src);
		d->size = s->size;
		d->data = new int64_t[d->size];
		for (
			auto
			from = reinterpret_cast<Object**>(s->data),
			to = reinterpret_cast<Object**>(d->data),
			term = from + d->size;
			from < term;
			from++, to++) {
			*to = Object::copy_object_field(*from);
		}
	}
	void Blob::copy_weak_array_fields(void* dst, void* src) {
		auto d = reinterpret_cast<Blob*>(dst);
		auto s = reinterpret_cast<Blob*>(src);
		d->size = s->size;
		d->data = new int64_t[d->size];
		auto to = reinterpret_cast<void**>(d->data);
		for (
			auto
			from = reinterpret_cast<Object::Weak**>(s->data),
			term = from + d->size;
			from < term;
			from++, to++) {
			Object::copy_weak_field(to, *from);
		}
	}
	void Blob::dispose_container(void* ptr) {
		auto p = reinterpret_cast<Blob*>(ptr);
		delete[] p->data;
	}
	void Blob::dispose_array(void* ptr) {
		auto p = reinterpret_cast<Blob*>(ptr);
		for (auto ptr = reinterpret_cast<Object**>(p->data), to = ptr + p->size; ptr < to; ptr++)
			Object::release(*ptr);
		delete[] p->data;
	}
	void Blob::dispose_weak_array(void* ptr) {
		auto p = reinterpret_cast<Blob*>(ptr);
		for (auto ptr = reinterpret_cast<Object::Weak**>(p->data), to = ptr + p->size; ptr < to; ptr++)
			Object::release_weak(*ptr);
		delete[] p->data;
	}

	bool Blob::to_str(StringObj* s, Blob* b, int at, int count) {
		if ((at + count) / sizeof(uint64_t) >= b->size)
			return false;
		if (s->buffer && --s->buffer->counter == 0)
			delete s->buffer;
		s->buffer = reinterpret_cast<StringObj::Buffer*>(new char[sizeof(StringObj::Buffer) + count]);
		s->buffer->counter = 1;
		memcpy(s->buffer->data, reinterpret_cast<char*>(b->data) + at, count);
		s->buffer->data[count] = 0;
		s->ptr = s->buffer->data;
		return true;
	}
	int64_t Blob::put_ch(Blob* b, int at, int codepoint) {
		int success = put_utf8(codepoint, [&](int byte) {
			if (at / sizeof(uint64_t) >= b->size)
				return false;
			reinterpret_cast<char*>(b->data)[at++] = byte;
			return true;
			});
		return success ? at : 0;
	}

	void register_content(struct ast::Ast& ast) {
		if (ast.object)
			return;
#ifdef STANDALONE_COMPILER_MODE
#define FN(A) (void(*)())(nullptr)
#else
		using FN = void(*)();
#endif
		auto sys = ast.dom->names()->get("sys");
		ast.object = ast.mk_class(sys->get("Object"));
		auto container = ast.mk_class(sys->get("Container"), {
			ast.mk_field(sys->get("_size"), new ast::ConstInt64),
			ast.mk_field(sys->get("_data"), new ast::ConstInt64) });
		ast.mk_fn(sys->get("Container")->get("size"), FN(&Blob::get_size), new ast::ConstInt64, { ast.get_ref(container) });
		ast.mk_fn(sys->get("Container")->get("insert"), FN(&Blob::insert_items), new ast::ConstVoid, { ast.get_ref(container), ast.tp_int64(), ast.tp_int64() });
		ast.mk_fn(sys->get("Container")->get("move"), FN(&Blob::move_array_items), new ast::ConstBool, { ast.get_ref(container), ast.tp_int64(), ast.tp_int64(), ast.tp_int64() });

		ast.blob = ast.mk_class(sys->get("Blob"));
		ast.blob->overloads[container];
		ast.mk_fn(sys->get("Blob")->get("getAt"), FN(&Blob::get_at), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
		ast.mk_fn(sys->get("Blob")->get("setAt"), FN(&Blob::set_at), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
		ast.mk_fn(sys->get("Blob")->get("getByteAt"), FN(&Blob::get_i8_at), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
		ast.mk_fn(sys->get("Blob")->get("setByteAt"), FN(&Blob::set_i8_at), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
		ast.mk_fn(sys->get("Blob")->get("get16At"), FN(&Blob::get_i16_at), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
		ast.mk_fn(sys->get("Blob")->get("set16At"), FN(&Blob::set_i16_at), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
		ast.mk_fn(sys->get("Blob")->get("get32At"), FN(&Blob::get_i32_at), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64() });
		ast.mk_fn(sys->get("Blob")->get("set32At"), FN(&Blob::set_i32_at), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
		ast.mk_fn(sys->get("Blob")->get("delete"), FN(&Blob::delete_blob_items), new ast::ConstVoid, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
		ast.mk_fn(sys->get("Blob")->get("copy"), FN(&Blob::blob_copy), new ast::ConstBool, { ast.get_ref(ast.blob), ast.tp_int64(), ast.get_ref(container), ast.tp_int64(), ast.tp_int64() });
		ast.mk_fn(sys->get("Blob")->get("putCh"), FN(&Blob::put_ch), new ast::ConstInt64, { ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
		ast.mk_fn(sys->get("terminate"), FN(&std::quick_exit), new ast::ConstVoid, {});

		auto inst = new ast::MkInstance;
		inst->cls = ast.object.pinned();
		auto ref_to_object = new ast::RefOp;
		ref_to_object->p = inst;
		auto opt_ref_to_object = new ast::If;
		opt_ref_to_object->p[0] = new ast::ConstBool;
		opt_ref_to_object->p[1] = ref_to_object;
		ast.own_array = ast.mk_class(sys->get("Array"));
		ast.own_array->overloads[container];
		ast.mk_fn(sys->get("Array")->get("getAt"), FN(&Blob::get_ref_at), opt_ref_to_object, { ast.get_ref(ast.own_array), ast.tp_int64() });
		ast.mk_fn(sys->get("Array")->get("setAt"), FN(&Blob::set_own_at), new ast::ConstVoid, { ast.get_ref(ast.own_array), ast.tp_int64(), ast.tp_optional(ast.object) });
		ast.mk_fn(sys->get("Array")->get("delete"), FN(&Blob::delete_array_items), new ast::ConstVoid, { ast.get_ref(ast.own_array), ast.tp_int64(), ast.tp_int64() });

		ast.weak_array = ast.mk_class(sys->get("WeakArray"));
		ast.weak_array->overloads[container];
		auto weak_to_object = new ast::MkWeakOp;
		weak_to_object->p = inst;
		ast.mk_fn(sys->get("WeakArray")->get("getAt"), FN(&Blob::get_weak_at), weak_to_object, { ast.get_ref(ast.weak_array), ast.tp_int64() });
		ast.mk_fn(sys->get("WeakArray")->get("setAt"), FN(&Blob::set_weak_at), new ast::ConstVoid, { ast.get_ref(ast.weak_array), ast.tp_int64(), ast.get_weak(ast.object) });
		ast.mk_fn(sys->get("WeakArray")->get("delete"), FN(&Blob::delete_weak_array_items), new ast::ConstVoid, { ast.get_ref(ast.weak_array), ast.tp_int64(), ast.tp_int64() });

		ast.string_cls = ast.mk_class(sys->get("String"), {
			ast.mk_field(sys->get("_cursor"), new ast::ConstInt64),
			ast.mk_field(sys->get("_buffer"), new ast::ConstInt64) });
		ast.mk_fn(sys->get("String")->get("fromBlob"), FN(&Blob::to_str), new ast::ConstBool, { ast.get_ref(ast.string_cls), ast.get_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
		ast.mk_fn(sys->get("String")->get("getCh"), FN(&StringObj::get_char), new ast::ConstInt64, { ast.get_ref(ast.string_cls) });
		ast.mk_fn(sys->get("makeShared"), FN(&Object::make_shared), new ast::ConstVoid, { ast.get_ref(ast.object) });  // TODO: its a hack till frozen objects were introduced

		ast.platform_exports.insert({
			{ "copy", FN(&Object::copy) },
			{ "copy_object_field", FN(&Object::copy_object_field) },
			{ "copy_weak_field", FN(&Object::copy_weak_field) },
			{ "release_weak", FN(&Object::release_weak) },
			{ "release", FN(&Object::release) },
			{ "alloc", FN(&Object::allocate) },
			{ "mk_weak", FN(&Object::mk_weak) },
			{ "deref_weak", FN(&Object::deref_weak) },
			{ "reg_copy_fixer", FN(&Object::reg_copy_fixer) },

			{ "sys_Container!copy", FN(&Blob::copy_container_fields) },
			{ "sys_Container!dtor", FN(&Blob::dispose_container) },
			{ "sys_Blob!copy", FN(&Blob::copy_container_fields) },
			{ "sys_Blob!dtor", FN(&Blob::dispose_container) },
			{ "sys_Array!copy", FN(&Blob::copy_array_fields) },
			{ "sys_Array!dtor",FN(&Blob::dispose_array) },
			{ "sys_WeakArray!copy", FN(&Blob::copy_weak_array_fields) },
			{ "sys_WeakArray!dtor", FN(&Blob::dispose_weak_array) },
			{ "sys_String!copy", FN(&StringObj::copy_fields) },
			{ "sys_String!dtor", FN(&StringObj::dispose_fields) }});
	}

} // namespace runtime
