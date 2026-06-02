#include "gc_internal.h"
static GcCardEntry* gc_card_table_find_entry(GcCardTable* table, const GcHeader* owner) {
size_t    i;
uintptr_t addr;
if (table == NULL || owner == NULL) {
return NULL;
}
addr = (uintptr_t)owner;
for (i = 0; i < table->count; ++i) {
GcCardEntry* entry = &table->entries[i];
if (entry->owner_base == addr) {
return entry;
}
}
return NULL;
}
void gc_card_table_init(GcCardTable* table, size_t card_granularity) {
if (table == NULL) {
return;
}
memset(table, 0, sizeof(*table));
table->card_granularity = (card_granularity != 0u) ? card_granularity : 256u;
}
void gc_card_table_destroy(GcCardTable* table) {
size_t i;
if (table == NULL) {
return;
}
for (i = 0; i < table->count; ++i) {
gc_bitmap_destroy(&table->entries[i].dirty_map);
}
free(table->entries);
memset(table, 0, sizeof(*table));
}
int gc_card_table_register_owner(GcRuntime* rt, GcCardTable* table, const GcHeader* owner, size_t owner_size) {
GcCardEntry* new_entries;
GcCardEntry* entry;
size_t       new_capacity;
size_t       card_count;
(void)rt;
if (table == NULL || owner == NULL || owner_size == 0u) {
return 0;
}
entry = gc_card_table_find_entry(table, owner);
if (entry != NULL) {
return 1;
}
if (table->count == table->capacity) {
new_capacity = (table->capacity > 0u) ? (table->capacity * 2u) : 32u;
new_entries = (GcCardEntry*)realloc(table->entries, new_capacity * sizeof(GcCardEntry));
if (new_entries == NULL) {
return 0;
}
table->entries = new_entries;
table->capacity = new_capacity;
}
card_count = (owner_size + table->card_granularity - 1u) / table->card_granularity;
if (card_count == 0u) {
card_count = 1u;
}
entry = &table->entries[table->count++];
memset(entry, 0, sizeof(*entry));
entry->owner_base = (uintptr_t)owner;
entry->owner_size = owner_size;
entry->card_count = card_count;
gc_bitmap_init(&entry->dirty_map, card_count);
if (entry->dirty_map.bit_count != card_count) {
table->count--;
return 0;
}
return 1;
}
void gc_card_table_unregister_owner(GcRuntime* rt, GcCardTable* table, const GcHeader* owner) {
size_t i;
if (table == NULL || owner == NULL) {
return;
}
for (i = 0; i < table->count; ++i) {
if (table->entries[i].owner_base == (uintptr_t)owner) {
if (rt != NULL && table->entries[i].dirty_card_count != 0u && rt->barriers.dirty_old_objects > 0u) {
rt->barriers.dirty_old_objects--;
}
if (rt != NULL && table->entries[i].dirty_card_count <= rt->barriers.dirty_cards) {
rt->barriers.dirty_cards -= table->entries[i].dirty_card_count;
}
gc_bitmap_destroy(&table->entries[i].dirty_map);
table->entries[i] = table->entries[table->count - 1u];
table->count--;
return;
}
}
}
void gc_card_table_mark_slot(GcRuntime* rt, GcCardTable* table, const GcHeader* owner, const void* slot_addr) {
GcCardEntry* entry;
size_t       offset;
size_t       card_index;
int          was_clean;
if (table == NULL || owner == NULL || slot_addr == NULL) {
return;
}
entry = gc_card_table_find_entry(table, owner);
if (entry == NULL || entry->card_count == 0u) {
return;
}
was_clean = (entry->dirty_card_count == 0u);
if ((uintptr_t)slot_addr <= entry->owner_base) {
card_index = 0u;
} else {
offset = (size_t)((uintptr_t)slot_addr - entry->owner_base);
card_index = offset / table->card_granularity;
}
if (card_index >= entry->card_count) {
card_index = entry->card_count - 1u;
}
if (!gc_bitmap_test(&entry->dirty_map, card_index)) {
gc_bitmap_set(&entry->dirty_map, card_index);
if (was_clean && rt != NULL) {
rt->barriers.dirty_old_objects++;
}
entry->dirty_card_count++;
if (rt != NULL) {
rt->barriers.dirty_cards++;
}
}
}
void gc_card_table_mark_owner(GcRuntime* rt, GcCardTable* table, const GcHeader* owner) {
GcCardEntry* entry;
size_t       i;
int          was_clean;
if (table == NULL || owner == NULL) {
return;
}
entry = gc_card_table_find_entry(table, owner);
if (entry == NULL) {
return;
}
was_clean = (entry->dirty_card_count == 0u);
for (i = 0; i < entry->card_count; ++i) {
if (!gc_bitmap_test(&entry->dirty_map, i)) {
gc_bitmap_set(&entry->dirty_map, i);
if (was_clean && rt != NULL) {
rt->barriers.dirty_old_objects++;
was_clean = 0;
}
entry->dirty_card_count++;
if (rt != NULL) {
rt->barriers.dirty_cards++;
}
}
}
}
int gc_card_table_owner_is_dirty(const GcCardTable* table, const GcHeader* owner) {
GcCardEntry* entry = gc_card_table_find_entry((GcCardTable*)table, owner);
return (entry != NULL) ? (entry->dirty_card_count != 0u) : 0;
}
void gc_card_table_clear_owner(GcRuntime* rt, GcCardTable* table, const GcHeader* owner) {
GcCardEntry* entry;
if (table == NULL || owner == NULL) {
return;
}
entry = gc_card_table_find_entry(table, owner);
if (entry == NULL || entry->dirty_card_count == 0u) {
return;
}
gc_bitmap_clear_all(&entry->dirty_map);
if (rt != NULL && rt->barriers.dirty_old_objects > 0u) {
rt->barriers.dirty_old_objects--;
}
if (rt != NULL && entry->dirty_card_count <= rt->barriers.dirty_cards) {
rt->barriers.dirty_cards -= entry->dirty_card_count;
}
entry->dirty_card_count = 0u;
}
void gc_card_table_visit_dirty(const GcCardTable* table, GcVisitDirtyCardFn visit, void* ctx) {
size_t i;
size_t card_index;
if (table == NULL || visit == NULL) {
return;
}
for (i = 0; i < table->count; ++i) {
const GcCardEntry* entry = &table->entries[i];
if (entry->dirty_card_count == 0u) {
continue;
}
for (card_index = 0; card_index < entry->card_count; ++card_index) {
if (gc_bitmap_test(&entry->dirty_map, card_index)) {
visit((GcHeader*)entry->owner_base, card_index, ctx);
}
}
}
}
