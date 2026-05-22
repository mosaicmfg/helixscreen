// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../catch_amalgamated.hpp"
#include "../helix_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "performance_state.h"
#include "ui_update_queue.h"

#include <lvgl.h>

using helix::perf::PerformanceState;
using helix::ui::UpdateQueue;
using helix::ui::UpdateQueueTestAccess;

namespace {

class PerfStateFixture : public HelixTestFixture {
  public:
    PerfStateFixture() {
        PerformanceState::instance().init_subjects();
    }
    ~PerfStateFixture() override {
        PerformanceState::instance().deinit_subjects();
    }
};

} // namespace

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState registers static subjects",
                 "[performance]") {
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_cpu_pct") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_cpu_pct_present") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_cpu_temp_c10") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_cpu_temp_present") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_mem_free_mb") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_mem_pct_used") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_mem_present") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_throttle_state") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_host_throttle_text") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_mcu_names") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_about_summary") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_available") != nullptr);
    REQUIRE(lv_xml_get_subject(nullptr, "perf_history_tick") != nullptr);

    // Defaults: nothing present, summary em-dash, available=0
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_available")) == 0);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_pct_present")) == 0);
}

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState applies sample and updates subjects",
                 "[performance]") {
    using helix::perf::PerfSample;

    PerfSample s;
    s.host_cpu_pct = 37.4f;
    s.host_cpu_temp_c = 61.4f;
    s.host_mem_free_mb = 812;
    s.host_mem_pct_used = 41.0f;
    s.host_throttle_bits = 0;

    PerformanceState::instance().push_sample_for_testing(s);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_pct")) == 37);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_pct_present")) == 1);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_temp_c10")) == 614);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_mem_free_mb")) == 812);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_mem_pct_used")) == 41);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_available")) == 1);
}

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState ring buffer fills then rolls",
                 "[performance]") {
    using helix::perf::PerfSample;
    auto& ps = PerformanceState::instance();

    for (int i = 0; i < 75; ++i) {
        PerfSample s;
        s.host_cpu_pct = static_cast<float>(i);
        ps.push_sample_for_testing(s);
    }
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    auto hist = ps.read_history("host_cpu_pct");
    REQUIRE(hist.size() == 60);
    REQUIRE(hist.front() == Catch::Approx(15.0f));   // oldest kept = i=15
    REQUIRE(hist.back()  == Catch::Approx(74.0f));   // newest = i=74
}

TEST_CASE_METHOD(PerfStateFixture,
                 "PerformanceState absent fields drop _present flags",
                 "[performance]") {
    using helix::perf::PerfSample;
    PerfSample s; // every optional empty
    PerformanceState::instance().push_sample_for_testing(s);
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_pct_present")) == 0);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_mem_present")) == 0);
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "perf_host_cpu_temp_present")) == 0);
}
