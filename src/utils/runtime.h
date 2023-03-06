#ifndef AK_RUNTIME_H_
#define AK_RUNTIME_H_

#include <cstdint>

#ifndef _MSC_VER
using std::size_t;  // MSVC has size_t not in std
#endif

namespace ast { struct Ast; }

namespace runtime {
	bool leak_detector_ok();

	enum Counter : std::uintptr_t {
		CTR_WEAKLESS = 1,
		CTR_FROZEN = 2,
		CTR_STEP = 0x10,
	};

	struct Object {
		struct Vmt {
			void (*copy_ref_fields)(void* dst, void* src);
			void (*dispose)(void* ptr);
			size_t instance_alloc_size;
			size_t vmt_size;
		};
		void** (*dispatcher)(uint64_t interface_and_method_ordinal);
		uintptr_t counter;  // pointer_to_weak_block || (number_of_owns_and_refs * CTR_STEP | CRT_WEAKLESS)

		static void release(Object* obj);
		static Object* retain(Object* obj);
		static void* allocate(size_t size);
		static uintptr_t get_ptr_tag(void* ptr) noexcept {
			return reinterpret_cast<uintptr_t>(ptr) & 3;
		}
		template <typename T> static T tag_ptr(void* ptr, uintptr_t tag) noexcept {
			return reinterpret_cast<T>(reinterpret_cast<uintptr_t>(ptr) | tag);
		}

		template <typename T> static T untag_ptr(void* ptr) noexcept {
			return reinterpret_cast<T>(reinterpret_cast<uintptr_t>(ptr) & ~3);
		}

		static Object* copy(Object* src);
		static Object* copy_object_field(Object* src);
		enum Tag : uintptr_t {
			TG_WEAK_BLOCK = 0,
			TG_OBJECT = 1,
			TG_WEAK = 2,
		};

		struct Weak {
			Object* target;
			int64_t wb_counter;   // number_of_weaks pointing here
			int64_t org_counter;  // copy of obj->counter
		};

		static void make_shared(Object* o);
		static Weak* retain_weak(Weak* w);
		static void release_weak(Weak* w);
		static void copy_weak_field(void** dst, Weak* src);
		static Weak* mk_weak(Object* obj);
		static Object* deref_weak(Weak* w);
		static void reg_copy_fixer(Object* object, void (*fixer)(Object*));
	};

	struct StringObj : Object {
		struct Buffer {
			int counter;
			char data[1];
		};
		const char* ptr;  // points to current char
		Buffer* buffer; // 0 for literals

		static int32_t get_char(StringObj* s);
		static void copy_fields(void* dst, void* src);
		static void dispose_fields(void* str);
	};

	struct Blob : Object {
		uint64_t size;
		int64_t* data;

		static int64_t get_size(Blob* b);
		static void insert_items(Blob* b, uint64_t index, uint64_t count);

		static void delete_blob_items(Blob* b, uint64_t index, uint64_t count);
		static void delete_array_items(Blob* b, uint64_t index, uint64_t count);
		static void delete_weak_array_items(Blob* b, uint64_t index, uint64_t count);

		static bool move_array_items(Blob* blob, uint64_t a, uint64_t b, uint64_t c);

		static int64_t get_at(Blob* b, uint64_t index);
		static void set_at(Blob* b, uint64_t index, int64_t val);
		static int64_t get_i8_at(Blob* b, uint64_t index);
		static void set_i8_at(Blob* b, uint64_t index, int64_t val);
		static int64_t get_i16_at(Blob* b, uint64_t index);
		static void set_i16_at(Blob* b, uint64_t index, int64_t val);
		static int64_t get_i32_at(Blob* b, uint64_t index);
		static void set_i32_at(Blob* b, uint64_t index, int64_t val);

		static bool blob_copy(Blob* dst, uint64_t dst_index, Blob* src, uint64_t src_index, uint64_t bytes);

		static Object* get_ref_at(Blob* b, uint64_t index);
		static Object::Weak* get_weak_at(Blob* b, uint64_t index);
		static void set_own_at(Blob* b, uint64_t index, Object* val);
		static void set_weak_at(Blob* b, uint64_t index, Object::Weak* val);

		static void copy_container_fields(void* dst, void* src);
		static void copy_array_fields(void* dst, void* src);
		static void copy_weak_array_fields(void* dst, void* src);

		static void dispose_container(void* ptr);
		static void dispose_array(void* ptr);
		static void dispose_weak_array(void* ptr);

		static bool to_str(StringObj* s, Blob* b, int at, int count);
		static int64_t put_ch(Blob* b, int at, int codepoint);
	};

	void register_content(struct ast::Ast&);
}

#endif // AK_RUNTIME_H_