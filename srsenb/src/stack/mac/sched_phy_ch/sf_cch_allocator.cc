/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "srsenb/hdr/stack/mac/sched_phy_ch/sf_cch_allocator.h"
#include "srsenb/hdr/stack/mac/sched_grid.h"
#include "srslte/srslog/bundled/fmt/format.h"

namespace srsenb {

bool is_pucch_sr_collision(const srslte_pucch_cfg_t& ue_pucch_cfg, tti_point tti_tx_dl_ack, uint32_t n1_pucch)
{
  if (ue_pucch_cfg.sr_configured && srslte_ue_ul_sr_send_tti(&ue_pucch_cfg, tti_tx_dl_ack.to_uint())) {
    return n1_pucch == ue_pucch_cfg.n_pucch_sr;
  }
  return false;
}

void sf_cch_allocator2::init(const sched_cell_params_t& cell_params_)
{
  cc_cfg           = &cell_params_;
  pucch_cfg_common = cc_cfg->pucch_cfg_common;
}

void sf_cch_allocator2::new_tti(tti_point tti_rx_)
{
  tti_rx = tti_rx_;

  dci_record_list.clear();
  last_dci_dfs.clear();
  current_cfix     = cc_cfg->sched_cfg->min_nof_ctrl_symbols - 1;
  current_max_cfix = cc_cfg->sched_cfg->max_nof_ctrl_symbols - 1;
}

const cce_cfi_position_table*
sf_cch_allocator2::get_cce_loc_table(alloc_type_t alloc_type, sched_ue* user, uint32_t cfix) const
{
  switch (alloc_type) {
    case alloc_type_t::DL_BC:
      return &cc_cfg->common_locations[cfix];
    case alloc_type_t::DL_PCCH:
      return &cc_cfg->common_locations[cfix];
    case alloc_type_t::DL_RAR:
      return &cc_cfg->rar_locations[to_tx_dl(tti_rx).sf_idx()][cfix];
    case alloc_type_t::DL_DATA:
      return user->get_locations(cc_cfg->enb_cc_idx, cfix + 1, to_tx_dl(tti_rx).sf_idx());
    case alloc_type_t::UL_DATA:
      return user->get_locations(cc_cfg->enb_cc_idx, cfix + 1, to_tx_dl(tti_rx).sf_idx());
    default:
      break;
  }
  return nullptr;
}

bool sf_cch_allocator2::alloc_dci(alloc_type_t alloc_type, uint32_t aggr_idx, sched_ue* user, bool has_pusch_grant)
{
  temp_dci_dfs.clear();
  uint32_t start_cfix = current_cfix;

  alloc_record record;
  record.user       = user;
  record.aggr_idx   = aggr_idx;
  record.alloc_type = alloc_type;
  record.pusch_uci  = has_pusch_grant;

  // Try to allocate grant. If it fails, attempt the same grant, but using a different permutation of past grant DCI
  // positions
  do {
    bool success = alloc_dfs_node(record, 0);
    if (success) {
      // DCI record allocation successful
      dci_record_list.push_back(record);

      if (is_dl_ctrl_alloc(alloc_type)) {
        // Dynamic CFI not yet supported for DL control allocations, as coderate can be exceeded
        current_max_cfix = current_cfix;
      }
      return true;
    }
    if (temp_dci_dfs.empty()) {
      temp_dci_dfs = last_dci_dfs;
    }
  } while (get_next_dfs());

  // Revert steps to initial state, before dci record allocation was attempted
  last_dci_dfs.swap(temp_dci_dfs);
  current_cfix = start_cfix;
  return false;
}

bool sf_cch_allocator2::get_next_dfs()
{
  do {
    uint32_t start_child_idx = 0;
    if (last_dci_dfs.empty()) {
      // If we reach root, increase CFI
      current_cfix++;
      if (current_cfix > current_max_cfix) {
        return false;
      }
    } else {
      // Attempt to re-add last tree node, but with a higher node child index
      start_child_idx = last_dci_dfs.back().dci_pos_idx + 1;
      last_dci_dfs.pop_back();
    }
    while (last_dci_dfs.size() < dci_record_list.size() and
           alloc_dfs_node(dci_record_list[last_dci_dfs.size()], start_child_idx)) {
      start_child_idx = 0;
    }
  } while (last_dci_dfs.size() < dci_record_list.size());

  // Finished computation of next DFS node
  return true;
}

bool sf_cch_allocator2::alloc_dfs_node(const alloc_record& record, uint32_t start_dci_idx)
{
  // Get DCI Location Table
  const cce_cfi_position_table* dci_locs = get_cce_loc_table(record.alloc_type, record.user, current_cfix);
  if (dci_locs == nullptr or (*dci_locs)[record.aggr_idx].empty()) {
    return false;
  }
  const cce_position_list& dci_pos_list = (*dci_locs)[record.aggr_idx];
  if (start_dci_idx >= dci_pos_list.size()) {
    return false;
  }

  tree_node node;
  node.dci_pos_idx = start_dci_idx;
  node.dci_pos.L   = record.aggr_idx;
  node.rnti        = record.user != nullptr ? record.user->get_rnti() : SRSLTE_INVALID_RNTI;
  node.current_mask.resize(nof_cces());
  // get cumulative pdcch & pucch masks
  if (not last_dci_dfs.empty()) {
    node.total_mask       = last_dci_dfs.back().total_mask;
    node.total_pucch_mask = last_dci_dfs.back().total_pucch_mask;
  } else {
    node.total_mask.resize(nof_cces());
    node.total_pucch_mask.resize(cc_cfg->nof_prb());
  }

  for (; node.dci_pos_idx < dci_pos_list.size(); ++node.dci_pos_idx) {
    node.dci_pos.ncce = dci_pos_list[node.dci_pos_idx];

    if (record.alloc_type == alloc_type_t::DL_DATA and not record.pusch_uci) {
      // The UE needs to allocate space in PUCCH for HARQ-ACK
      pucch_cfg_common.n_pucch = node.dci_pos.ncce + pucch_cfg_common.N_pucch_1;

      if (is_pucch_sr_collision(record.user->get_ue_cfg().pucch_cfg, to_tx_dl_ack(tti_rx), pucch_cfg_common.n_pucch)) {
        // avoid collision of HARQ-ACK with own SR n(1)_pucch
        continue;
      }

      node.pucch_n_prb = srslte_pucch_n_prb(&cc_cfg->cfg.cell, &pucch_cfg_common, 0);
      if (not cc_cfg->sched_cfg->pucch_mux_enabled and node.total_pucch_mask.test(node.pucch_n_prb)) {
        // PUCCH allocation would collide with other PUCCH/PUSCH grants. Try another CCE position
        continue;
      }
    }

    node.current_mask.reset();
    node.current_mask.fill(node.dci_pos.ncce, node.dci_pos.ncce + (1U << record.aggr_idx));
    if ((node.total_mask & node.current_mask).any()) {
      // there is a PDCCH collision. Try another CCE position
      continue;
    }

    // Allocation successful
    node.total_mask |= node.current_mask;
    if (node.pucch_n_prb >= 0) {
      node.total_pucch_mask.set(node.pucch_n_prb);
    }
    last_dci_dfs.push_back(node);
    return true;
  }

  return false;
}

void sf_cch_allocator2::rem_last_dci()
{
  assert(not dci_record_list.empty());

  // Remove DCI record
  last_dci_dfs.pop_back();
  dci_record_list.pop_back();
}

void sf_cch_allocator2::get_allocs(alloc_result_t* vec, pdcch_mask_t* tot_mask, size_t idx) const
{
  if (vec != nullptr) {
    vec->clear();

    vec->resize(last_dci_dfs.size());
    for (uint32_t i = 0; i < last_dci_dfs.size(); ++i) {
      (*vec)[i] = &last_dci_dfs[i];
    }
  }

  if (tot_mask != nullptr) {
    if (last_dci_dfs.empty()) {
      tot_mask->resize(nof_cces());
      tot_mask->reset();
    } else {
      *tot_mask = last_dci_dfs.back().total_mask;
    }
  }
}

std::string sf_cch_allocator2::result_to_string(bool verbose) const
{
  fmt::basic_memory_buffer<char, 1024> strbuf;
  if (dci_record_list.empty()) {
    fmt::format_to(strbuf, "SCHED: PDCCH allocations cfi={}, nof_cce={}, No allocations.\n", get_cfi(), nof_cces());
  } else {
    fmt::format_to(strbuf,
                   "SCHED: PDCCH allocations cfi={}, nof_cce={}, nof_allocs={}, total PDCCH mask=0x{:x}",
                   get_cfi(),
                   nof_cces(),
                   nof_allocs(),
                   last_dci_dfs.back().total_mask);
    alloc_result_t vec;
    get_allocs(&vec);
    if (verbose) {
      fmt::format_to(strbuf, ", allocations:\n");
      for (const auto& dci_alloc : vec) {
        fmt::format_to(strbuf,
                       "  > rnti=0x{:0x}: 0x{:x} / 0x{:x}\n",
                       dci_alloc->rnti,
                       dci_alloc->current_mask,
                       dci_alloc->total_mask);
      }
    } else {
      fmt::format_to(strbuf, ".\n");
    }
  }
  return fmt::to_string(strbuf);
}

/////////////////////////

void sf_cch_allocator::init(const sched_cell_params_t& cell_params_)
{
  cc_cfg           = &cell_params_;
  pucch_cfg_common = cc_cfg->pucch_cfg_common;

  // init alloc trees
  alloc_trees.reserve(cc_cfg->sched_cfg->max_nof_ctrl_symbols);
  for (uint32_t i = 0; i < cc_cfg->sched_cfg->max_nof_ctrl_symbols; ++i) {
    alloc_trees.emplace_back(i + 1, *cc_cfg, pucch_cfg_common);
  }
}

void sf_cch_allocator::new_tti(tti_point tti_rx_)
{
  tti_rx = tti_rx_;

  // Reset back all CFIs
  for (auto& t : alloc_trees) {
    t.reset();
  }
  dci_record_list.clear();
  current_cfix     = cc_cfg->sched_cfg->min_nof_ctrl_symbols - 1;
  current_max_cfix = cc_cfg->sched_cfg->max_nof_ctrl_symbols - 1;
}

const cce_cfi_position_table*
sf_cch_allocator::get_cce_loc_table(alloc_type_t alloc_type, sched_ue* user, uint32_t cfix) const
{
  switch (alloc_type) {
    case alloc_type_t::DL_BC:
      return &cc_cfg->common_locations[cfix];
    case alloc_type_t::DL_PCCH:
      return &cc_cfg->common_locations[cfix];
    case alloc_type_t::DL_RAR:
      return &cc_cfg->rar_locations[to_tx_dl(tti_rx).sf_idx()][cfix];
    case alloc_type_t::DL_DATA:
      return user->get_locations(cc_cfg->enb_cc_idx, cfix + 1, to_tx_dl(tti_rx).sf_idx());
    case alloc_type_t::UL_DATA:
      return user->get_locations(cc_cfg->enb_cc_idx, cfix + 1, to_tx_dl(tti_rx).sf_idx());
    default:
      break;
  }
  return nullptr;
}

bool sf_cch_allocator::alloc_dci(alloc_type_t alloc_type, uint32_t aggr_idx, sched_ue* user, bool has_pusch_grant)
{
  // TODO: Make the alloc tree update lazy
  alloc_record_t record{.user = user, .aggr_idx = aggr_idx, .alloc_type = alloc_type, .pusch_uci = has_pusch_grant};

  if (is_dl_ctrl_alloc(alloc_type) and nof_allocs() == 0 and current_max_cfix > current_cfix) {
    // Given that CFI is not currently dynamic for ctrl allocs, in case of SIB/RAR alloc, start with optimal CFI
    // in terms of nof CCE locs
    uint32_t nof_locs = 0, lowest_cfix = current_cfix;
    for (uint32_t cfix_tmp = current_max_cfix; cfix_tmp > lowest_cfix; --cfix_tmp) {
      const cce_cfi_position_table* dci_locs = get_cce_loc_table(record.alloc_type, record.user, cfix_tmp);
      if ((*dci_locs)[record.aggr_idx].size() > nof_locs) {
        nof_locs     = (*dci_locs)[record.aggr_idx].size();
        current_cfix = cfix_tmp;
      } else {
        break;
      }
    }
  }

  // Try to allocate user in PDCCH for given CFI. If it fails, increment CFI.
  uint32_t first_cfi = get_cfi();
  bool     success;
  do {
    success = alloc_dci_record(record, get_cfi() - 1);
  } while (not success and current_cfix < current_max_cfix and set_cfi(get_cfi() + 1));

  if (not success) {
    // DCI allocation failed. go back to original CFI
    if (get_cfi() != first_cfi and not set_cfi(first_cfi)) {
      logger.error("SCHED: Failed to return back to original PDCCH state");
    }
    return false;
  }

  // DCI record allocation successful
  dci_record_list.push_back(record);

  if (is_dl_ctrl_alloc(alloc_type)) {
    // Dynamic CFI not yet supported for DL control allocations, as coderate can be exceeded
    current_max_cfix = current_cfix;
  }

  return true;
}

void sf_cch_allocator::rem_last_dci()
{
  assert(not dci_record_list.empty());

  // Remove DCI record
  dci_record_list.pop_back();

  // Remove leaves of PDCCH position decisions
  auto& tree    = alloc_trees[current_cfix];
  tree.prev_end = tree.prev_start;
  if (dci_record_list.empty()) {
    tree.prev_start = 0;
  } else {
    tree.prev_start = tree.dci_alloc_tree[tree.prev_start].parent_idx;
    // Discover other tree nodes with same level
    while (tree.prev_start > 0) {
      uint32_t count      = 1;
      int      parent_idx = tree.dci_alloc_tree[tree.prev_start - 1].parent_idx;
      while (parent_idx >= 0) {
        count++;
        parent_idx = tree.dci_alloc_tree[parent_idx].parent_idx;
      }
      if (count == dci_record_list.size()) {
        tree.prev_start--;
      } else {
        break;
      }
    }
  }
  tree.dci_alloc_tree.erase(tree.dci_alloc_tree.begin() + tree.prev_end, tree.dci_alloc_tree.end());
}

bool sf_cch_allocator::alloc_dci_record(const alloc_record_t& record, uint32_t cfix)
{
  bool  ret  = false;
  auto& tree = alloc_trees[cfix];

  // Get DCI Location Table
  const cce_cfi_position_table* dci_locs = get_cce_loc_table(record.alloc_type, record.user, cfix);
  if (dci_locs == nullptr or (*dci_locs)[record.aggr_idx].empty()) {
    return ret;
  }

  if (tree.prev_end > 0) {
    for (size_t j = tree.prev_start; j < tree.prev_end; ++j) {
      ret |= tree.add_tree_node_leaves((int)j, record, *dci_locs, tti_rx);
    }
  } else {
    ret = tree.add_tree_node_leaves(-1, record, *dci_locs, tti_rx);
  }

  if (ret) {
    tree.prev_start = tree.prev_end;
    tree.prev_end   = tree.dci_alloc_tree.size();
  }

  return ret;
}

bool sf_cch_allocator::set_cfi(uint32_t cfi)
{
  if (cfi < cc_cfg->sched_cfg->min_nof_ctrl_symbols or cfi > cc_cfg->sched_cfg->max_nof_ctrl_symbols) {
    logger.error("Invalid CFI value. Defaulting to current CFI.");
    return false;
  }

  uint32_t new_cfix = cfi - 1;
  if (new_cfix == current_cfix) {
    return true;
  }

  // setup new PDCCH alloc tree
  auto& new_tree = alloc_trees[new_cfix];
  new_tree.reset();

  if (not dci_record_list.empty()) {
    // there are already PDCCH allocations

    // Rebuild Allocation Tree
    bool ret = true;
    for (const auto& old_record : dci_record_list) {
      ret &= alloc_dci_record(old_record, new_cfix);
    }

    if (not ret) {
      // Fail to rebuild allocation tree. Go back to previous CFI
      return false;
    }
  }

  current_cfix = new_cfix;
  return true;
}

void sf_cch_allocator::get_allocs(alloc_result_t* vec, pdcch_mask_t* tot_mask, size_t idx) const
{
  alloc_trees[current_cfix].get_allocs(vec, tot_mask, idx);
}

std::string sf_cch_allocator::result_to_string(bool verbose) const
{
  return alloc_trees[current_cfix].result_to_string(verbose);
}

sf_cch_allocator::alloc_tree_t::alloc_tree_t(uint32_t                   this_cfi,
                                             const sched_cell_params_t& cc_params,
                                             srslte_pucch_cfg_t&        pucch_cfg_common) :
  cfi(this_cfi), cc_cfg(&cc_params), pucch_cfg_temp(&pucch_cfg_common), nof_cces(cc_params.nof_cce_table[this_cfi - 1])
{
  dci_alloc_tree.reserve(8);
}

void sf_cch_allocator::alloc_tree_t::reset()
{
  prev_start = 0;
  prev_end   = 0;
  dci_alloc_tree.clear();
}

/// Algorithm to compute a valid PDCCH allocation
bool sf_cch_allocator::alloc_tree_t::add_tree_node_leaves(int                           parent_node_idx,
                                                          const alloc_record_t&         dci_record,
                                                          const cce_cfi_position_table& dci_locs,
                                                          tti_point                     tti_rx_)
{
  bool ret = false;

  alloc_t alloc;
  alloc.rnti      = (dci_record.user != nullptr) ? dci_record.user->get_rnti() : SRSLTE_INVALID_RNTI;
  alloc.dci_pos.L = dci_record.aggr_idx;

  // get cumulative pdcch & pucch masks
  pdcch_mask_t parent_total_mask;
  prbmask_t    parent_pucch_mask;
  if (parent_node_idx >= 0) {
    parent_total_mask = dci_alloc_tree[parent_node_idx].node.total_mask;
    parent_pucch_mask = dci_alloc_tree[parent_node_idx].node.total_pucch_mask;
  } else {
    parent_total_mask.resize(nof_cces);
    parent_pucch_mask.resize(cc_cfg->nof_prb());
  }

  for (uint32_t i = 0; i < dci_locs[dci_record.aggr_idx].size(); ++i) {
    int8_t   pucch_prbidx = -1;
    uint32_t ncce_pos     = dci_locs[dci_record.aggr_idx][i];

    if (dci_record.alloc_type == alloc_type_t::DL_DATA and not dci_record.pusch_uci) {
      // The UE needs to allocate space in PUCCH for HARQ-ACK
      pucch_cfg_temp->n_pucch = ncce_pos + pucch_cfg_temp->N_pucch_1;

      if (is_pucch_sr_collision(
              dci_record.user->get_ue_cfg().pucch_cfg, to_tx_dl_ack(tti_rx_), pucch_cfg_temp->n_pucch)) {
        // avoid collision of HARQ-ACK with own SR n(1)_pucch
        continue;
      }

      pucch_prbidx = srslte_pucch_n_prb(&cc_cfg->cfg.cell, pucch_cfg_temp, 0);
      if (not cc_cfg->sched_cfg->pucch_mux_enabled and parent_pucch_mask.test(pucch_prbidx)) {
        // PUCCH allocation would collide with other PUCCH/PUSCH grants. Try another CCE position
        continue;
      }
    }

    pdcch_mask_t alloc_mask(nof_cces);
    alloc_mask.fill(ncce_pos, ncce_pos + (1u << dci_record.aggr_idx));
    if ((parent_total_mask & alloc_mask).any()) {
      // there is a PDCCH collision. Try another CCE position
      continue;
    }

    // Allocation successful
    alloc.current_mask     = alloc_mask;
    alloc.total_mask       = parent_total_mask | alloc_mask;
    alloc.dci_pos.ncce     = ncce_pos;
    alloc.pucch_n_prb      = pucch_prbidx;
    alloc.total_pucch_mask = parent_pucch_mask;
    if (pucch_prbidx >= 0) {
      alloc.total_pucch_mask.set(pucch_prbidx);
    }

    // Prune if repetition of total_masks
    uint32_t j = prev_end;
    for (; j < dci_alloc_tree.size(); ++j) {
      if (dci_alloc_tree[j].node.total_mask == alloc.total_mask) {
        // leave nested for-loop
        break;
      }
    }
    if (j < dci_alloc_tree.size()) {
      continue;
    }

    // Register allocation
    dci_alloc_tree.emplace_back(parent_node_idx, alloc);
    ret = true;
  }

  return ret;
}

void sf_cch_allocator::alloc_tree_t::get_allocs(alloc_result_t* vec, pdcch_mask_t* tot_mask, size_t idx) const
{
  // if alloc tree is empty
  if (prev_start == prev_end) {
    if (vec != nullptr) {
      vec->clear();
    }
    if (tot_mask != nullptr) {
      tot_mask->resize(nof_cces);
      tot_mask->reset();
    }
    return;
  }

  // set vector of allocations
  if (vec != nullptr) {
    vec->clear();
    size_t i = prev_start + idx;
    while (dci_alloc_tree[i].parent_idx >= 0) {
      vec->push_back(&dci_alloc_tree[i].node);
      i = (size_t)dci_alloc_tree[i].parent_idx;
    }
    vec->push_back(&dci_alloc_tree[i].node);
    std::reverse(vec->begin(), vec->end());
  }

  // set final cce mask
  if (tot_mask != nullptr) {
    *tot_mask = dci_alloc_tree[prev_start + idx].node.total_mask;
  }
}

std::string sf_cch_allocator::alloc_tree_t::result_to_string(bool verbose) const
{
  // get all the possible combinations of DCI pos allocations
  fmt::basic_memory_buffer<char, 1024> strbuf;
  fmt::format_to(strbuf,
                 "SCHED: PDCCH allocations cfi={}, nof_cce={}, {} possible combinations:\n",
                 cfi,
                 nof_cces,
                 prev_end - prev_start);
  uint32_t count = 0;
  for (size_t i = prev_start; i < prev_end; ++i) {
    alloc_result_t vec;
    pdcch_mask_t   tot_mask;
    get_allocs(&vec, &tot_mask, i - prev_start);

    fmt::format_to(strbuf, "[{}]: total mask=0x{:x}", count, tot_mask);
    if (verbose) {
      fmt::format_to(strbuf, ", allocations:\n");
      for (const auto& dci_alloc : vec) {
        fmt::format_to(strbuf,
                       "  > rnti=0x{:0x}: 0x{:x} / 0x{:x}\n",
                       dci_alloc->rnti,
                       dci_alloc->current_mask,
                       dci_alloc->total_mask);
      }
    } else {
      fmt::format_to(strbuf, "\n");
    }
    count++;
  }
  return fmt::to_string(strbuf);
}

} // namespace srsenb
