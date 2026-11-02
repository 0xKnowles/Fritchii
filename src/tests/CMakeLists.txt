# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/event.cpp
    common/queue_timed_wait.cpp
    common/queue_lifetime.cpp
    common/fibers.cpp
    common/host_memory.cpp
    common/host_memory_fallback.cpp
    common/param_package.cpp
    common/paged_scalar.cpp
    common/range_map.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/slot_vector.cpp
    common/undefined_fix.cpp
    common/unique_function.cpp
    core/core_timing.cpp
    core/address_space.cpp
    core/page_heap.cpp
    core/read_traps.cpp
    core/rasterizer_write.cpp
    core/nx2_readback_cache.cpp
    core/nx2_core_readback_capacity.cpp
    core/nx2_display.cpp
    core/nso.cpp
    core/nrr.cpp
    core/ncz_decompressor.cpp
    core/internal_network/network.cpp
    video_core/memory_tracker.cpp
    video_core/descriptor_table.cpp
    video_core/native_async_draw.cpp
    video_core/memory_budget.cpp
    video_core/astc_recompression_override.cpp
    video_core/nx2_video_configuration.cpp
    video_core/nx2_decoder_image.cpp
    video_core/nx2_present.cpp
    video_core/nx2_gpu_mappings.cpp
    video_core/nx2_sparse_mappings.cpp
    video_core/nx2_sparse_region.cpp
    video_core/nx2_sparse_tiles.cpp
    video_core/nx2_sparse_images.cpp
    video_core/nx2_sparse_image_layout.cpp
    video_core/nx2_sparse_image_write.cpp
    video_core/nx2_command_sync.cpp
    video_core/semaphore_acquire.cpp
    video_core/nx2_command_ranges.cpp
    video_core/physical_alias.cpp
    video_core/ampere_returns.cpp
    video_core/ampere_f2i.cpp
    video_core/compute_storage_buffers.cpp
    video_core/compute_dispatch.cpp
    video_core/gpu_split_metrics.cpp
    video_core/draw_table_snapshot.cpp
    video_core/download_batch.cpp
    video_core/virtual_range_cache.cpp
    video_core/uniform_upload_cache.cpp
    video_core/uniform_cache_history.cpp
    video_core/buffer_address_guards.cpp
    video_core/storage_binding_sync.cpp
    video_core/command_write_frontier.cpp
    video_core/split_completion.cpp
    video_core/split_front_handoff.cpp
    video_core/split_publication_order.cpp
    video_core/readback_timing.cpp
    video_core/pending_fence_readback.cpp
    video_core/fence_download_merge.cpp
    video_core/texture_audit_collector.cpp
    video_core/sync_audit_command_fetch.cpp
    video_core/readback_audit.cpp
    video_core/native_storage_mappings.cpp
    video_core/native_qmd_write.cpp
    video_core/readback_coverage.cpp
    video_core/retired_resource.cpp
    video_core/cooperative_matrix.cpp
    video_core/ampere_kirby.cpp
    video_core/ampere_geometry.cpp
    video_core/ampere_geometry_lowering.cpp
    video_core/ampere_geometry_exit.cpp
    video_core/ampere_uniform_lea.cpp
    video_core/ampere_integer_multiply.cpp
    video_core/ampere_async_copy.cpp
    video_core/buffer_texture.cpp
    video_core/shader_reuse.cpp
    video_core/native_root_table.cpp
    video_core/ampere_matrix.cpp
    video_core/ampere_trap_guards.cpp
    video_core/ampere_program_override.cpp
    video_core/x8d24_conversion.cpp
    video_core/ampere_bindless_tex.cpp
    video_core/ampere_sured.cpp
    video_core/ampere_release_guards.cpp
    video_core/ampere_release_f2i.cpp
    input_common/calibration_configuration_job.cpp
)

create_target_directory_groups(tests)

if (WIN32)
    target_sources(tests PRIVATE common/host_memory_commit.cpp)
endif()

target_link_libraries(tests PRIVATE common core input_common video_core)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)

add_test(NAME tests COMMAND tests)

# needed for vma
if (NOT MSVC)
    target_compile_options(tests PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()
