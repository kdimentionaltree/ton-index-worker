#include "DataParser.h"
#include "td/utils/common.h"
#include "crypto/common/refcnt.hpp"
#include "crypto/block/block.h"
#include "crypto/block/transaction.h"
#include "crypto/block/block-auto.h"
#include "crypto/block/block-parse.h"
#include "validator/interfaces/block.h"
#include "validator/interfaces/shard.h"
#include "convert-utils.h"

using namespace ton::validator; //TODO: remove this

void ParseQuery::start_up() {
  auto status = parse_impl();
  if(status.is_error()) {
    promise_.set_error(status.move_as_error());
  }
  else {
    promise_.set_result(std::move(result));
  }
  stop();
}

td::Status ParseQuery::parse_impl() {
  td::optional<schema::Block> mc_block;
  for (auto &block_ds : mc_block_) {
    // common block info
    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    block::gen::BlockExtra::Record extra;
    if (!(tlb::unpack_cell(block_ds.block_data->root_cell(), blk) && tlb::unpack_cell(blk.info, info) && tlb::unpack_cell(blk.extra, extra))) {
        return td::Status::Error("block data info extra unpack failed");
    }

    // block details
    auto schema_block = parse_block(block_ds.block_data->block_id(), blk, info, extra, mc_block);
    if (!mc_block) {
        mc_block = schema_block;
    }

    // transactions and messages
    std::set<td::Bits256> addresses;
    TRY_RESULT_ASSIGN(schema_block.transactions, parse_transactions(block_ds.block_data->block_id(), blk, info, extra, addresses));

    // account states
    TRY_STATUS(parse_account_states(block_ds.block_state, addresses));

    result->blocks_.push_back(schema_block);
  }
  result->mc_block_ = mc_block_;
  return td::Status::OK();
}

schema::Block ParseQuery::parse_block(const ton::BlockIdExt& blk_id, block::gen::Block::Record& blk, const block::gen::BlockInfo::Record& info, 
                          const block::gen::BlockExtra::Record& extra, td::optional<schema::Block> &mc_block) {
  schema::Block block;
  block.workchain = blk_id.id.workchain;
  block.shard = static_cast<int64_t>(blk_id.id.shard);
  block.seqno = blk_id.id.seqno;
  block.root_hash = td::base64_encode(blk_id.root_hash.as_slice());
  block.file_hash = td::base64_encode(blk_id.file_hash.as_slice());
  if (mc_block) {
      block.mc_block_workchain = mc_block.value().workchain;
      block.mc_block_shard = mc_block.value().shard;
      block.mc_block_seqno = mc_block.value().seqno;
  }
  block.global_id = blk.global_id;
  block.version = info.version;
  block.after_merge = info.after_merge;
  block.before_split = info.before_split;
  block.after_split = info.after_split;
  block.want_split = info.want_split;
  block.key_block = info.key_block;
  block.vert_seqno_incr = info.vert_seqno_incr;
  block.flags = info.flags;
  block.gen_utime = info.gen_utime;
  block.start_lt = info.start_lt;
  block.end_lt = info.end_lt;
  block.validator_list_hash_short = info.gen_validator_list_hash_short;
  block.gen_catchain_seqno = info.gen_catchain_seqno;
  block.min_ref_mc_seqno = info.min_ref_mc_seqno;
  block.key_block = info.key_block;
  block.prev_key_block_seqno = info.prev_key_block_seqno;
  block.vert_seqno = info.vert_seq_no;
  block::gen::ExtBlkRef::Record mcref{};
  if (!info.not_master || tlb::unpack_cell(info.master_ref, mcref)) {
      block.master_ref_seqno = mcref.seq_no;
  }
  block.rand_seed = td::base64_encode(extra.rand_seed.as_slice());
  block.created_by = td::base64_encode(extra.created_by.as_slice());
  return block;
}

td::Result<schema::Message> ParseQuery::parse_message(td::Ref<vm::Cell> msg_cell) {
  schema::Message msg;
  msg.hash = msg_cell->get_hash().bits();

  block::gen::Message::Record message;
  if (!tlb::type_unpack_cell(msg_cell, block::gen::t_Message_Any, message)) {
    return td::Status::Error("Failed to unpack Message");
  }

  td::Ref<vm::CellSlice> body;
  if (message.body->prefetch_long(1) == 0) {
    body = std::move(message.body);
    body.write().advance(1);
  } else {
    body = vm::load_cell_slice_ref(message.body->prefetch_ref());
  }
  msg.body = vm::CellBuilder().append_cellslice(*body).finalize();

  TRY_RESULT(body_boc, convert::to_bytes(msg.body));
  if (!body_boc) {
    return td::Status::Error("Failed to convert message body to bytes");
  }
  msg.body_boc = body_boc.value();

  if (body->prefetch_long(32) != vm::CellSlice::fetch_long_eof) {
    msg.opcode = body->prefetch_long(32);
  }

  td::Ref<vm::Cell> init_state_cell;
  auto& init_state_cs = message.init.write();
  if (init_state_cs.fetch_ulong(1) == 1) {
    if (init_state_cs.fetch_long(1) == 0) {
      msg.init_state = vm::CellBuilder().append_cellslice(init_state_cs).finalize();
    } else {
      msg.init_state = init_state_cs.fetch_ref();
    }
    TRY_RESULT(init_state_boc, convert::to_bytes(msg.init_state));
    if (!init_state_boc) {
      return td::Status::Error("Failed to convert message init state to bytes");
    }
    msg.init_state_boc = init_state_boc.value();
  }
      
  auto tag = block::gen::CommonMsgInfo().get_tag(*message.info);
  if (tag < 0) {
    return td::Status::Error("Failed to read CommonMsgInfo tag");
  }
  switch (tag) {
    case block::gen::CommonMsgInfo::int_msg_info: {
      block::gen::CommonMsgInfo::Record_int_msg_info msg_info;
      if (!tlb::csr_unpack(message.info, msg_info)) {
        return td::Status::Error("Failed to unpack CommonMsgInfo::int_msg_info");
      }

      TRY_RESULT_ASSIGN(msg.value, convert::to_balance(msg_info.value));
      TRY_RESULT_ASSIGN(msg.source, convert::to_raw_address(msg_info.src));
      TRY_RESULT_ASSIGN(msg.destination, convert::to_raw_address(msg_info.dest));
      TRY_RESULT_ASSIGN(msg.fwd_fee, convert::to_balance(msg_info.fwd_fee));
      TRY_RESULT_ASSIGN(msg.ihr_fee, convert::to_balance(msg_info.ihr_fee));
      msg.created_lt = msg_info.created_lt;
      msg.created_at = msg_info.created_at;
      msg.bounce = msg_info.bounce;
      msg.bounced = msg_info.bounced;
      msg.ihr_disabled = msg_info.ihr_disabled;
      return msg;
    }
    case block::gen::CommonMsgInfo::ext_in_msg_info: {
      block::gen::CommonMsgInfo::Record_ext_in_msg_info msg_info;
      if (!tlb::csr_unpack(message.info, msg_info)) {
        return td::Status::Error("Failed to unpack CommonMsgInfo::ext_in_msg_info");
      }
      
      // msg.source = null, because it is external
      TRY_RESULT_ASSIGN(msg.destination, convert::to_raw_address(msg_info.dest))
      TRY_RESULT_ASSIGN(msg.import_fee, convert::to_balance(msg_info.import_fee));
      return msg;
    }
    case block::gen::CommonMsgInfo::ext_out_msg_info: {
      block::gen::CommonMsgInfo::Record_ext_out_msg_info msg_info;
      if (!tlb::csr_unpack(message.info, msg_info)) {
        return td::Status::Error("Failed to unpack CommonMsgInfo::ext_out_msg_info");
      }
      TRY_RESULT_ASSIGN(msg.source, convert::to_raw_address(msg_info.src));
      // msg.destination = null, because it is external
      msg.created_lt = static_cast<uint64_t>(msg_info.created_lt);
      msg.created_at = static_cast<uint32_t>(msg_info.created_at);
      return msg;
    }
  }

  return td::Status::Error("Unknown CommonMsgInfo tag");
}

td::Result<schema::TrStoragePhase> ParseQuery::parse_tr_storage_phase(vm::CellSlice& cs) {
  block::gen::TrStoragePhase::Record phase_data;
  if (!tlb::unpack(cs, phase_data)) {
    return td::Status::Error("Failed to unpack TrStoragePhase");
  }
  schema::TrStoragePhase phase;
  TRY_RESULT_ASSIGN(phase.storage_fees_collected, convert::to_balance(phase_data.storage_fees_collected));
  auto& storage_fees_due = phase_data.storage_fees_due.write();
  if (storage_fees_due.fetch_ulong(1) == 1) {
    TRY_RESULT_ASSIGN(phase.storage_fees_due, convert::to_balance(storage_fees_due));
  }
  phase.status_change = static_cast<schema::AccStatusChange>(phase_data.status_change);
  return phase;
}

td::Result<schema::TrCreditPhase> ParseQuery::parse_tr_credit_phase(vm::CellSlice& cs) {
  block::gen::TrCreditPhase::Record phase_data;
  if (!tlb::unpack(cs, phase_data)) {
    return td::Status::Error("Failed to unpack TrCreditPhase");
  }
  schema::TrCreditPhase phase;
  auto& due_fees_collected = phase_data.due_fees_collected.write();
  if (due_fees_collected.fetch_ulong(1) == 1) {
    TRY_RESULT_ASSIGN(phase.due_fees_collected, convert::to_balance(due_fees_collected));
  }
  TRY_RESULT_ASSIGN(phase.credit, convert::to_balance(phase_data.credit));
  return phase;
}

td::Result<schema::TrComputePhase> ParseQuery::parse_tr_compute_phase(vm::CellSlice& cs) {
  int compute_ph_tag = block::gen::t_TrComputePhase.get_tag(cs);
  if (compute_ph_tag == block::gen::TrComputePhase::tr_phase_compute_vm) {
    block::gen::TrComputePhase::Record_tr_phase_compute_vm compute_vm;
    if (!tlb::unpack(cs, compute_vm)) {
      return td::Status::Error("Error unpacking tr_phase_compute_vm");
    }
    schema::TrComputePhase_vm res;
    res.success = compute_vm.success;
    res.msg_state_used = compute_vm.msg_state_used;
    res.account_activated = compute_vm.account_activated;
    TRY_RESULT_ASSIGN(res.gas_fees, convert::to_balance(compute_vm.gas_fees));
    res.gas_used = block::tlb::t_VarUInteger_7.as_uint(*compute_vm.r1.gas_used);
    res.gas_limit = block::tlb::t_VarUInteger_7.as_uint(*compute_vm.r1.gas_limit);
    auto& gas_credit = compute_vm.r1.gas_credit.write();
    if (gas_credit.fetch_ulong(1)) {
      res.gas_credit = block::tlb::t_VarUInteger_3.as_uint(gas_credit);
    }
    res.mode = compute_vm.r1.mode;
    res.exit_code = compute_vm.r1.exit_code;
    auto& exit_arg = compute_vm.r1.exit_arg.write();
    if (exit_arg.fetch_ulong(1)) {
      res.exit_arg = exit_arg.fetch_long(32);
    }
    res.vm_steps = compute_vm.r1.vm_steps;
    res.vm_init_state_hash = compute_vm.r1.vm_init_state_hash;
    res.vm_final_state_hash = compute_vm.r1.vm_final_state_hash;
    return res;
  } else if (compute_ph_tag == block::gen::TrComputePhase::tr_phase_compute_skipped) {
    block::gen::TrComputePhase::Record_tr_phase_compute_skipped skip;
    if (!tlb::unpack(cs, skip)) {
      return td::Status::Error("Error unpacking tr_phase_compute_skipped");
    }
    return schema::TrComputePhase_skipped{static_cast<schema::ComputeSkipReason>(skip.reason)};
  }
  return td::Status::OK();
}

td::Result<schema::StorageUsedShort> ParseQuery::parse_storage_used_short(vm::CellSlice& cs) {
  block::gen::StorageUsedShort::Record info;
  if (!tlb::unpack(cs, info)) {
    return td::Status::Error("Error unpacking StorageUsedShort");
  }
  schema::StorageUsedShort res;
  res.bits = block::tlb::t_VarUInteger_7.as_uint(*info.bits);
  res.cells = block::tlb::t_VarUInteger_7.as_uint(*info.cells);
  return res;
}

td::Result<schema::TrActionPhase> ParseQuery::parse_tr_action_phase(vm::CellSlice& cs) {
  block::gen::TrActionPhase::Record info;
  if (!tlb::unpack(cs, info)) {
    return td::Status::Error("Error unpacking TrActionPhase");
  }
  schema::TrActionPhase res;
  res.success = info.success;
  res.valid = info.valid;
  res.no_funds = info.no_funds;
  res.status_change = static_cast<schema::AccStatusChange>(info.status_change);
  auto& total_fwd_fees = info.total_fwd_fees.write();
  if (total_fwd_fees.fetch_ulong(1) == 1) {
    TRY_RESULT_ASSIGN(res.total_fwd_fees, convert::to_balance(info.total_fwd_fees));
  }
  auto& total_action_fees = info.total_action_fees.write();
  if (total_action_fees.fetch_ulong(1) == 1) {
    TRY_RESULT_ASSIGN(res.total_action_fees, convert::to_balance(info.total_action_fees));
  }
  res.result_code = info.result_code;
  auto& result_arg = info.result_arg.write();
  if (result_arg.fetch_ulong(1)) {
    res.result_arg = result_arg.fetch_long(32);
  }
  res.tot_actions = info.tot_actions;
  res.spec_actions = info.spec_actions;
  res.skipped_actions = info.skipped_actions;
  res.msgs_created = info.msgs_created;
  res.action_list_hash = info.action_list_hash;
  TRY_RESULT_ASSIGN(res.tot_msg_size, parse_storage_used_short(info.tot_msg_size.write()));
  return res;
}

td::Result<schema::TrBouncePhase> ParseQuery::parse_tr_bounce_phase(vm::CellSlice& cs) {
  int bounce_ph_tag = block::gen::t_TrBouncePhase.get_tag(cs);
  switch (bounce_ph_tag) {
    case block::gen::TrBouncePhase::tr_phase_bounce_negfunds: {
      block::gen::TrBouncePhase::Record_tr_phase_bounce_negfunds negfunds;
      if (!tlb::unpack(cs, negfunds)) {
        return td::Status::Error("Error unpacking tr_phase_bounce_negfunds");
      }
      return schema::TrBouncePhase_negfunds();
    }
    case block::gen::TrBouncePhase::tr_phase_bounce_nofunds: {
      block::gen::TrBouncePhase::Record_tr_phase_bounce_nofunds nofunds;
      if (!tlb::unpack(cs, nofunds)) {
        return td::Status::Error("Error unpacking tr_phase_bounce_nofunds");
      }
      schema::TrBouncePhase_nofunds res;
      TRY_RESULT_ASSIGN(res.msg_size, parse_storage_used_short(nofunds.msg_size.write()));
      TRY_RESULT_ASSIGN(res.req_fwd_fees, convert::to_balance(nofunds.req_fwd_fees));
      return res;
    }
    case block::gen::TrBouncePhase::tr_phase_bounce_ok: {
      block::gen::TrBouncePhase::Record_tr_phase_bounce_ok ok;
      if (!tlb::unpack(cs, ok)) {
        return td::Status::Error("Error unpacking tr_phase_bounce_ok");
      }
      schema::TrBouncePhase_ok res;
      TRY_RESULT_ASSIGN(res.msg_size, parse_storage_used_short(ok.msg_size.write()));
      TRY_RESULT_ASSIGN(res.msg_fees, convert::to_balance(ok.msg_fees));
      TRY_RESULT_ASSIGN(res.fwd_fees, convert::to_balance(ok.fwd_fees));
      return res;
    }
    default:
      return td::Status::Error("Unknown TrBouncePhase tag");
  }
}

td::Result<schema::SplitMergeInfo> ParseQuery::parse_split_merge_info(td::Ref<vm::CellSlice>& cs) {
  block::gen::SplitMergeInfo::Record info;
  if (!tlb::csr_unpack(cs, info)) {
    return td::Status::Error("Error unpacking SplitMergeInfo");
  }
  schema::SplitMergeInfo res;
  res.cur_shard_pfx_len = info.cur_shard_pfx_len;
  res.acc_split_depth = info.acc_split_depth;
  res.this_addr = info.this_addr;
  res.sibling_addr = info.sibling_addr;
  return res;
}

td::Result<schema::TransactionDescr> ParseQuery::process_transaction_descr(vm::CellSlice& td_cs) {
  auto tag = block::gen::t_TransactionDescr.get_tag(td_cs);
  switch (tag) {
    case block::gen::TransactionDescr::trans_ord: {
      block::gen::TransactionDescr::Record_trans_ord ord;
      if (!tlb::unpack_exact(td_cs, ord)) {
        return td::Status::Error("Error unpacking trans_ord");
      }
      schema::TransactionDescr_ord res;
      res.credit_first = ord.credit_first;
      auto& storage_ph = ord.storage_ph.write();
      if (storage_ph.fetch_ulong(1) == 1) {
        TRY_RESULT_ASSIGN(res.storage_ph, parse_tr_storage_phase(storage_ph));
      }
      auto& credit_ph = ord.credit_ph.write();
      if (credit_ph.fetch_ulong(1) == 1) {
        TRY_RESULT_ASSIGN(res.credit_ph, parse_tr_credit_phase(credit_ph));
      }
      TRY_RESULT_ASSIGN(res.compute_ph, parse_tr_compute_phase(ord.compute_ph.write()));
      auto& action = ord.action.write();
      if (action.fetch_ulong(1) == 1) {
        auto action_cs = vm::load_cell_slice(action.fetch_ref());
        TRY_RESULT_ASSIGN(res.action, parse_tr_action_phase(action_cs));
      }
      res.aborted = ord.aborted;
      auto& bounce = ord.bounce.write();
      if (bounce.fetch_ulong(1)) {
        TRY_RESULT_ASSIGN(res.bounce, parse_tr_bounce_phase(bounce));
      }
      res.destroyed = ord.destroyed;
      return res;
    }
    case block::gen::TransactionDescr::trans_storage: {
      block::gen::TransactionDescr::Record_trans_storage storage;
      if (!tlb::unpack_exact(td_cs, storage)) {
        return td::Status::Error("Error unpacking trans_storage");
      }
      schema::TransactionDescr_storage res;
      TRY_RESULT_ASSIGN(res.storage_ph, parse_tr_storage_phase(storage.storage_ph.write()));
      return res;
    }
    case block::gen::TransactionDescr::trans_tick_tock: {
      block::gen::TransactionDescr::Record_trans_tick_tock tick_tock;
      if (!tlb::unpack_exact(td_cs, tick_tock)) {
        return td::Status::Error("Error unpacking trans_tick_tock");
      }
      schema::TransactionDescr_tick_tock res;
      res.is_tock = tick_tock.is_tock;
      TRY_RESULT_ASSIGN(res.storage_ph, parse_tr_storage_phase(tick_tock.storage_ph.write()));
      TRY_RESULT_ASSIGN(res.compute_ph, parse_tr_compute_phase(tick_tock.compute_ph.write()));
      auto& action = tick_tock.action.write();
      if (action.fetch_ulong(1) == 1) {
        auto action_cs = vm::load_cell_slice(action.fetch_ref());
        TRY_RESULT_ASSIGN(res.action, parse_tr_action_phase(action_cs));
      }
      res.aborted = tick_tock.aborted;
      res.destroyed = tick_tock.destroyed;
      return res;
    }
    case block::gen::TransactionDescr::trans_split_prepare: {
      block::gen::TransactionDescr::Record_trans_split_prepare split_prepare;
      if (!tlb::unpack_exact(td_cs, split_prepare)) {
        return td::Status::Error("Error unpacking trans_split_prepare");
      }
      schema::TransactionDescr_split_prepare res;
      TRY_RESULT_ASSIGN(res.split_info, parse_split_merge_info(split_prepare.split_info));
      auto& storage_ph = split_prepare.storage_ph.write();
      if (storage_ph.fetch_ulong(1)) {
        TRY_RESULT_ASSIGN(res.storage_ph, parse_tr_storage_phase(storage_ph));
      }
      TRY_RESULT_ASSIGN(res.compute_ph, parse_tr_compute_phase(split_prepare.compute_ph.write()));
      auto& action = split_prepare.action.write();
      if (action.fetch_ulong(1)) {
        auto action_cs = vm::load_cell_slice(action.fetch_ref());
        TRY_RESULT_ASSIGN(res.action, parse_tr_action_phase(action_cs));
      }
      res.aborted = split_prepare.aborted;
      res.destroyed = split_prepare.destroyed;
      return res;
    }
    case block::gen::TransactionDescr::trans_split_install: {
      block::gen::TransactionDescr::Record_trans_split_install split_install;
      if (!tlb::unpack_exact(td_cs, split_install)) {
        return td::Status::Error("Error unpacking trans_split_install");
      }
      schema::TransactionDescr_split_install res;
      TRY_RESULT_ASSIGN(res.split_info, parse_split_merge_info(split_install.split_info));
      res.installed = split_install.installed;
      return res;
    }
    case block::gen::TransactionDescr::trans_merge_prepare: {
      block::gen::TransactionDescr::Record_trans_merge_prepare merge_prepare;
      if (!tlb::unpack_exact(td_cs, merge_prepare)) {
        return td::Status::Error("Error unpacking trans_merge_prepare");
      }
      schema::TransactionDescr_merge_prepare res;
      TRY_RESULT_ASSIGN(res.split_info, parse_split_merge_info(merge_prepare.split_info));
      TRY_RESULT_ASSIGN(res.storage_ph, parse_tr_storage_phase(merge_prepare.storage_ph.write()));
      res.aborted = merge_prepare.aborted;
      return res;
    }
    case block::gen::TransactionDescr::trans_merge_install: {
      block::gen::TransactionDescr::Record_trans_merge_install merge_install;
      if (!tlb::unpack_exact(td_cs, merge_install)) {
        return td::Status::Error("Error unpacking trans_merge_install");
      }
      schema::TransactionDescr_merge_install res;
      TRY_RESULT_ASSIGN(res.split_info, parse_split_merge_info(merge_install.split_info));
      auto& storage_ph = merge_install.storage_ph.write();
      if (storage_ph.fetch_ulong(1)) {
        TRY_RESULT_ASSIGN(res.storage_ph, parse_tr_storage_phase(storage_ph));
      }
      auto& credit_ph = merge_install.credit_ph.write();
      if (credit_ph.fetch_ulong(1)) {
        TRY_RESULT_ASSIGN(res.credit_ph, parse_tr_credit_phase(credit_ph));
      }
      TRY_RESULT_ASSIGN(res.compute_ph, parse_tr_compute_phase(merge_install.compute_ph.write()));
      auto& action = merge_install.action.write();
      if (action.fetch_ulong(1)) {
        auto action_cs = vm::load_cell_slice(action.fetch_ref());
        TRY_RESULT_ASSIGN(res.action, parse_tr_action_phase(action_cs));
      }
      res.aborted = merge_install.aborted;
      res.destroyed = merge_install.destroyed;
      return res;
    }
    default:
      return td::Status::Error("Unknown transaction description type");
  }
}

td::Result<std::vector<schema::Transaction>> ParseQuery::parse_transactions(const ton::BlockIdExt& blk_id, const block::gen::Block::Record &block, 
                              const block::gen::BlockInfo::Record &info, const block::gen::BlockExtra::Record &extra,
                              std::set<td::Bits256> &addresses) {
  std::vector<schema::Transaction> res;
  try {
    vm::AugmentedDictionary acc_dict{vm::load_cell_slice_ref(extra.account_blocks), 256, block::tlb::aug_ShardAccountBlocks};

    td::Bits256 cur_addr = td::Bits256::zero();
    bool eof = false;
    bool allow_same = true;
    while (!eof) {
      td::Ref<vm::CellSlice> value;
      try {
        value = acc_dict.extract_value(
            acc_dict.vm::DictionaryFixed::lookup_nearest_key(cur_addr.bits(), 256, true, allow_same));
      } catch (vm::VmError err) {
        return td::Status::Error(PSLICE() << "error while traversing account block dictionary: " << err.get_msg());
      }
      if (value.is_null()) {
        eof = true;
        break;
      }
      allow_same = false;
      block::gen::AccountBlock::Record acc_blk;
      if (!(tlb::csr_unpack(std::move(value), acc_blk) && acc_blk.account_addr == cur_addr)) {
        return td::Status::Error("invalid AccountBlock for account " + cur_addr.to_hex());
      }
      vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                                          block::tlb::aug_AccountTransactions};
      td::BitArray<64> cur_trans{(long long)0};
      while (true) {
        td::Ref<vm::Cell> tvalue;
        try {
          tvalue = trans_dict.extract_value_ref(
              trans_dict.vm::DictionaryFixed::lookup_nearest_key(cur_trans.bits(), 64, true));
        } catch (vm::VmError err) {
          return td::Status::Error(PSLICE() << "error while traversing transaction dictionary of an AccountBlock: " << err.get_msg());
        }
        if (tvalue.is_null()) {
          break;
        }
        block::gen::Transaction::Record trans;
        if (!tlb::unpack_cell(tvalue, trans)) {
          return td::Status::Error("Failed to unpack Transaction");
        }

        schema::Transaction schema_tx;

        schema_tx.account = block::StdAddress(blk_id.id.workchain, cur_addr);
        schema_tx.hash = tvalue->get_hash().bits();
        schema_tx.lt = trans.lt;
        schema_tx.prev_trans_hash = trans.prev_trans_hash;
        schema_tx.prev_trans_lt = trans.prev_trans_lt;
        schema_tx.now = trans.now;

        schema_tx.orig_status = static_cast<schema::AccountStatus>(trans.orig_status);
        schema_tx.end_status = static_cast<schema::AccountStatus>(trans.end_status);

        TRY_RESULT_ASSIGN(schema_tx.total_fees, convert::to_balance(trans.total_fees));

        if (trans.r1.in_msg->prefetch_long(1)) {
          auto msg = trans.r1.in_msg->prefetch_ref();
          TRY_RESULT_ASSIGN(schema_tx.in_msg, parse_message(trans.r1.in_msg->prefetch_ref()));
        }

        if (trans.outmsg_cnt != 0) {
          vm::Dictionary dict{trans.r1.out_msgs, 15};
          for (int x = 0; x < trans.outmsg_cnt; x++) {
            TRY_RESULT(out_msg, parse_message(dict.lookup_ref(td::BitArray<15>{x})));
            schema_tx.out_msgs.push_back(std::move(out_msg));
          }
        }

        block::gen::HASH_UPDATE::Record state_hash_update;
        if (!tlb::type_unpack_cell(std::move(trans.state_update), block::gen::t_HASH_UPDATE_Account, state_hash_update)) {
          return td::Status::Error("Failed to unpack state_update");
        }
        
        schema_tx.account_state_hash_before = state_hash_update.old_hash;
        schema_tx.account_state_hash_after = state_hash_update.new_hash;

        auto descr_cs = vm::load_cell_slice(trans.description);
        TRY_RESULT_ASSIGN(schema_tx.description, process_transaction_descr(descr_cs));

        res.push_back(schema_tx);

        addresses.insert(cur_addr);
      }
    }
  } catch (vm::VmError err) {
      return td::Status::Error(PSLICE() << "error while parsing AccountBlocks : " << err.get_msg());
  }
  return res;
}

td::Status ParseQuery::parse_account_states(const td::Ref<ShardState>& block_state, std::set<td::Bits256> &addresses) {
  auto root = block_state->root_cell();
  block::gen::ShardStateUnsplit::Record sstate;
  if (!tlb::unpack_cell(root, sstate)) {
    return td::Status::Error("Failed to unpack ShardStateUnsplit");
  }
  vm::AugmentedDictionary accounts_dict{vm::load_cell_slice_ref(sstate.accounts), 256, block::tlb::aug_ShardAccounts};
  for (auto &addr : addresses) {
    auto shard_account_csr = accounts_dict.lookup(addr);
    if (shard_account_csr.is_null()) {
      // account is uninitialized after this block
      continue;
    } 
    td::Ref<vm::Cell> account_root = shard_account_csr->prefetch_ref();

    int account_tag = block::gen::t_Account.get_tag(vm::load_cell_slice(account_root));
    switch (account_tag) {
    case block::gen::Account::account_none:
      continue;
    case block::gen::Account::account: {
      TRY_RESULT(account, parse_account(std::move(account_root)));
      result->account_states_.push_back(account);
      break;
    }
    default:
      return td::Status::Error("Unknown account tag");
    }
  }
  LOG(DEBUG) << "Parsed " << result->account_states_.size() << " account states";
  return td::Status::OK();
}

td::Result<schema::AccountState> ParseQuery::parse_account(td::Ref<vm::Cell> account_root) {
  block::gen::Account::Record_account account;
  if (!tlb::unpack_cell(account_root, account)) {
    return td::Status::Error("Failed to unpack Account");
  }
  block::gen::AccountStorage::Record storage;
  if (!tlb::csr_unpack(account.storage, storage)) {
    return td::Status::Error("Failed to unpack AccountStorage");
  }

  schema::AccountState schema_account;
  schema_account.hash = account_root->get_hash().bits();
  TRY_RESULT(account_addr, convert::to_raw_address(account.addr));
  TRY_RESULT_ASSIGN(schema_account.account, block::StdAddress::parse(account_addr));
  TRY_RESULT_ASSIGN(schema_account.balance, convert::to_balance(storage.balance));
  schema_account.last_trans_lt = storage.last_trans_lt;

  int account_state_tag = block::gen::t_AccountState.get_tag(storage.state.write());
  switch (account_state_tag) {
    case block::gen::AccountState::account_uninit:
      schema_account.account_status = "uninit";
      break;
    case block::gen::AccountState::account_frozen: {
      schema_account.account_status = "frozen";
      block::gen::AccountState::Record_account_frozen frozen;
      if (!tlb::csr_unpack(storage.state, frozen)) {
        return td::Status::Error("Failed to unpack AccountState frozen");
      }
      schema_account.frozen_hash = td::base64_encode(frozen.state_hash.as_slice());
      break;
    }
    case block::gen::AccountState::account_active: {
      schema_account.account_status = "active";
      block::gen::AccountState::Record_account_active active;
      if (!tlb::csr_unpack(storage.state, active)) {
        return td::Status::Error("Failed to unpack AccountState active");
      }
      block::gen::StateInit::Record state_init;
      if (!tlb::csr_unpack(active.x, state_init)) {
        return td::Status::Error("Failed to unpack StateInit");
      }
      auto& code_cs = state_init.code.write();
      if (code_cs.fetch_long(1) != 0) {
        schema_account.code = code_cs.prefetch_ref();
        schema_account.code_hash = td::base64_encode(schema_account.code->get_hash().as_slice());
      }
      auto& data_cs = state_init.data.write();
      if (data_cs.fetch_long(1) != 0) {
        schema_account.data = data_cs.prefetch_ref();
        schema_account.data_hash = td::base64_encode(schema_account.data->get_hash().as_slice());
      }
      break;
    }
    default:
      return td::Status::Error("Unknown account state tag");
  }
  return schema_account;
}
