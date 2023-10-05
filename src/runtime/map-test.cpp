#include "utils/fake-gunit.h"
#include "runtime/map.h"

namespace {

void DummyCopier(void* dst, void* src) {
}
int disposes = 0;
void DummyDisposer(void* obj) {
	disposes++;
}
void DummyVisitor(
	void* ptr,
	void(*visitor)(
		void*,  // field_ptr*
		int,    // type AG_VISIT_*
		void*), // ctx
	void* ctx) {
}

AgVmt dummy_vmt = { DummyCopier, DummyDisposer, DummyVisitor, sizeof(AgObject), sizeof(AgVmt) };
AgVmt map_vmt = { ag_copy_sys_Map, ag_dtor_sys_Map, DummyVisitor, sizeof(AgMap), sizeof(AgVmt) };

AgObject* mk_object() {
	AgObject* r = ag_allocate_obj(sizeof(AgObject));
	r->dispatcher = (ag_dispatcher_t)(&dummy_vmt + 1);
	return r;
}

AgMap* mk_map() {
	AgMap* r = (AgMap*) ag_allocate_obj(sizeof(AgMap));
	r->head.dispatcher = (ag_dispatcher_t)(&map_vmt + 1);
	return r;
}

TEST(Map, Simple) {
	AgMap* map = mk_map();
	AgObject* k1 = mk_object();
	AgObject* k2 = mk_object();
	AgObject* v1 = mk_object();
	ag_m_sys_Map_set(map, k1, v1);
	ASSERT_EQ(ag_m_sys_Map_get(map, k1), v1);
	ag_release_pin(v1);
	ASSERT_EQ(ag_m_sys_Map_get(map, k2), nullptr);
	ag_release_pin(k1);
	ag_release_pin(k2);
	ag_release_pin(v1);
	ag_release_pin(&map->head);
	ASSERT_TRUE(ag_leak_detector_ok());
}

// TODO: add map tests

} // namespace
