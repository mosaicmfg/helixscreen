// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "performance_state.h"

#include "static_subject_registry.h"
#include "subject_managed_panel.h"

#include <cstdio>
#include <spdlog/spdlog.h>

namespace helix {
namespace perf {

PerformanceState& PerformanceState::instance() {
    static PerformanceState s;
    return s;
}

void PerformanceState::init_subjects() {
    if (initialized_) {
        return;
    }

    UI_MANAGED_SUBJECT_INT(s_host_cpu_pct_,         0, "perf_host_cpu_pct",         subjects_);
    UI_MANAGED_SUBJECT_INT(s_host_cpu_pct_present_,  0, "perf_host_cpu_pct_present", subjects_);
    UI_MANAGED_SUBJECT_INT(s_host_cpu_temp_c10_,     0, "perf_host_cpu_temp_c10",    subjects_);
    UI_MANAGED_SUBJECT_INT(s_host_cpu_temp_present_, 0, "perf_host_cpu_temp_present",subjects_);
    UI_MANAGED_SUBJECT_INT(s_host_mem_free_mb_,      0, "perf_host_mem_free_mb",     subjects_);
    UI_MANAGED_SUBJECT_INT(s_host_mem_pct_used_,     0, "perf_host_mem_pct_used",    subjects_);
    UI_MANAGED_SUBJECT_INT(s_host_mem_present_,      0, "perf_host_mem_present",     subjects_);
    UI_MANAGED_SUBJECT_INT(s_host_throttle_state_,   0, "perf_host_throttle_state",  subjects_);
    UI_MANAGED_SUBJECT_STRING(s_host_throttle_text_, buf_throttle_text_, "",
                              "perf_host_throttle_text", subjects_);
    UI_MANAGED_SUBJECT_STRING(s_mcu_names_, buf_mcu_names_, "",
                              "perf_mcu_names", subjects_);
    UI_MANAGED_SUBJECT_STRING(s_about_summary_, buf_about_summary_, "\xe2\x80\x94",
                              "perf_about_summary", subjects_);
    UI_MANAGED_SUBJECT_INT(s_available_,    0, "perf_available",    subjects_);
    UI_MANAGED_SUBJECT_INT(s_history_tick_, 0, "perf_history_tick", subjects_);

    StaticSubjectRegistry::instance().register_deinit(
        "PerformanceState", []() { PerformanceState::instance().deinit_subjects(); });

    initialized_ = true;
}

void PerformanceState::deinit_subjects() {
    if (!initialized_) {
        return;
    }
    if (source_) {
        source_->stop();
        source_.reset();
    }
    lifetime_.invalidate();
    {
        std::lock_guard<std::mutex> lk(history_mu_);
        history_.clear();
    }
    mcu_subjects_.clear();
    subjects_.deinit_all();
    initialized_ = false;
}

void PerformanceState::set_source(std::unique_ptr<IPerformanceSource>) {
    // Wired in Task 9
}

std::vector<float> PerformanceState::read_history(const std::string&) const {
    return {}; // Wired in Task 3
}

void PerformanceState::push_sample_for_testing(const PerfSample& s) {
    apply_sample(s); // Wired in Task 3
}

void PerformanceState::apply_sample(const PerfSample&) { /* Task 3 */ }
void PerformanceState::update_about_summary() { /* Task 4 */ }
void PerformanceState::update_mcu_subjects(const std::vector<McuStat>&) { /* Task 5 */ }
void PerformanceState::push_history(const std::string&, float) { /* Task 3 */ }

std::string PerformanceState::mcu_safe_name(const std::string& raw) {
    std::string out = raw;
    for (auto& c : out) {
        if (c == ' ' || c == '/' || c == '.') c = '_';
    }
    return out;
}

} // namespace perf
} // namespace helix
