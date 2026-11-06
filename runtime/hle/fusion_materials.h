#pragma once
#include "../armor_fusion_policy.h"
#include "../cpu.h"
#include <stdbool.h>

// Game-thread only. Selection changes scratch models, never inventory or gear.
bool fusion_materials_open(Cpu *cpu, uint32_t smith, uint16_t category);
int fusion_materials_poll(Cpu *cpu, FusionSelection *out, size_t capacity, size_t *count);
// Teardown must discard cached addresses without writes if the parent died.
void fusion_materials_cancel(Cpu *cpu, bool restore);
