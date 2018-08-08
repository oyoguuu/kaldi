// fstext/context-fst.cc

// Copyright      2018  Daniel Povey

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.

#include "fstext/context-fst.h"
#include "base/kaldi-error.h"

namespace fst {


InverseContextFst::InverseContextFst(
    Label subsequential_symbol,
    const vector<int32>& phones,
    const vector<int32>& disambig_syms,
    int32 context_width,
    int32 central_position):
    context_width_(context_width),
    central_position_(central_position),
    phone_syms_(phones),
    disambig_syms_(disambig_syms),
    subsequential_symbol_(subsequential_symbol) {

  {  // This block checks the inputs.
    KALDI_ASSERT(subsequential_symbol != 0
                 && disambig_syms_.count(subsequential_symbol) == 0
                 && phone_syms_.count(subsequential_symbol) == 0);
    if (phone_syms_.empty())
      KALDI_WARN << "Context FST created but there are no phone symbols: probably "
          "input FST was empty.";
    KALDI_ASSERT(phone_syms_.count(0) == 0 && disambig_syms_.count(0) == 0 &&
                 central_position_ >= 0 && central_position_ < context_width_);
    for (size_t i = 0; i < phones.size(); i++) {
      KALDI_ASSERT(disambig_syms_.count(phones[i]) == 0);
    }
  }

  // empty vector, will be the ilabel_info vector that corresponds to epsilon,
  // in case our FST needs to output epsilons.
  vector<int32> empty_vec;
  Label epsilon_label = FindLabel(empty_vec);

  // epsilon_vec is the phonetic context window we have at the very start of a
  // sequence, meaning "no real phones have been seen yet".
  vector<int32> epsilon_vec(context_width_ - 1, 0);
  StateId start_state = FindState(epsilon_vec);

  KALDI_ASSERT(epsilon_label == 0 && start_state == 0);

  if (context_width_ > central_position_ + 1 && !disambig_syms_.empty()) {
    // We add a symbol whose sequence representation is [ 0 ], and whose
    // symbol-id is 1.  This is treated as a disambiguation symbol, we call it
    // #-1 in printed form.  It is necessary to ensure that all determinizable
    // LG's will have determinizable CLG's.  The problem it fixes is quite
    // subtle-- it relates to reordering of disambiguation symbols (they appear
    // earlier in CLG than in LG, relative to phones), and the fact that if a
    // disambig symbol appears at the very start of a sequence in CLG, it's not
    // clear exatly where it appeared on the corresponding sequence at the input
    // of LG.
    vector<int32> pseudo_eps_vec;
    pseudo_eps_vec.push_back(0);
    pseudo_eps_symbol_= FindLabel(pseudo_eps_vec);
    KALDI_ASSERT(pseudo_eps_symbol_ == 1);
  } else {
    pseudo_eps_symbol_ = 0;  // use actual epsilon.
  }
}


void InverseContextFst::ShiftSequenceLeft(Label label,
                                          std::vector<int32> *phone_seq) {
  if (!phone_seq->empty()) {
    phone_seq->erase(phone_seq->begin());
    phone_seq->push_back(label);
  }
}

void InverseContextFst::GetFullPhoneSequence(
    const std::vector<int32> &seq, Label label,
    std::vector<int32> *full_phone_sequence) {
  int32 context_width = context_width_;
  full_phone_sequence->reserve(context_width);
  full_phone_sequence->insert(full_phone_sequence->end(),
                              seq.begin(), seq.end());
  full_phone_sequence->push_back(label);
  for (int32 i = central_position_ + 1; i < context_width; i++) {
    if ((*full_phone_sequence)[i] == subsequential_symbol_) {
      (*full_phone_sequence)[i] = 0;
    }
  }
}


InverseContextFst::Weight InverseContextFst::Final(StateId s) {
  KALDI_ASSERT(static_cast<size_t>(s) < state_seqs_.size());

  const vector<int32> &phone_context = state_seqs_[s];

  KALDI_ASSERT(phone_context.size() == context_width_ - 1);

  bool has_final_prob;

  if (central_position_ < context_width_ - 1) {
    has_final_prob = (phone_context[central_position_] == subsequential_symbol_);
    // if phone_context[central_position_] != subsequential_symbol_ then we have
    // pending phones-in-context that we still need to output, so we need to
    // consume more subsequential symbols before we can terminate.
  } else {
    has_final_prob = true;
  }
  return has_final_prob ? Weight::One() : Weight::Zero();
}

bool InverseContextFst::GetArc(StateId s, Label ilabel, Arc *arc) {
  KALDI_ASSERT(ilabel != 0 && static_cast<size_t>(s) < state_seqs_.size() &&
               state_seqs_[s].size() == context_width_ - 1);

  if (IsDisambigSymbol(ilabel)) {
    // A disambiguation-symbol self-loop arc.
    CreateDisambigArc(s, ilabel, arc);
    return true;
  } else if (IsPhoneSymbol(ilabel)) {
    const vector<int32> &seq = state_seqs_[s];
    if (!seq.empty() && seq.back() == subsequential_symbol_) {
      return false;  // A real phone is not allowed to follow the subsequential
                     // symbol.
    }

    // next_seq will be 'seq' shifted left by 1, with 'ilabel' appended.
    vector<int32> next_seq(seq);
    ShiftSequenceLeft(ilabel, &next_seq);

    // full-seq will be the full context window of size context_width_.
    vector<int32> full_seq;
    GetFullPhoneSequence(seq, ilabel, &full_seq);

    StateId next_s = FindState(next_seq);

    CreatePhoneOrEpsArc(s, next_s, ilabel, full_seq, arc);
    return true;
  } else if (ilabel == subsequential_symbol_) {
    const vector<int32> &seq = state_seqs_[s];

    if (central_position_ + 1 == context_width_ ||
        seq[central_position_] == subsequential_symbol_) {
      // We already had "enough" subsequential symbols in a row and don't want to
      // accept any more, or we'd be making the subsequential symbol the central phone.
      return false;
    }

    // full-seq will be the full context window of size context_width_.
    vector<int32> full_seq;
    GetFullPhoneSequence(seq, ilabel, &full_seq);

    vector<int32> next_seq(seq);
    ShiftSequenceLeft(ilabel, &next_seq);
    StateId next_s = FindState(next_seq);

    CreatePhoneOrEpsArc(s, next_s, ilabel, full_seq, arc);
    return true;
  } else {
    KALDI_ERR << "ContextFst: CreateArc, invalid ilabel supplied [confusion "
              << "about phone list or disambig symbols?]: " << ilabel;
  }
  return false;  // won't get here.  suppress compiler error.
}


void InverseContextFst::CreateDisambigArc(StateId s, Label ilabel, Arc *arc) {
  // Creates a self-loop arc corresponding to the disambiguation symbol.
  vector<int32> label_info;       // This will be a vector containing just [ -olabel ].
  label_info.push_back(-ilabel);  // olabel is a disambiguation symbol.  Use its negative
                                  // so we can more easily distinguish them from phones.
  Label olabel = FindLabel(label_info);
  arc->ilabel = ilabel;
  arc->olabel = olabel;
  arc->weight = Weight::One();
  arc->nextstate = s;  // self-loop.
}

void InverseContextFst::CreatePhoneOrEpsArc(StateId src, StateId dest,
                                            Label ilabel,
                                            const vector<int32> &phone_seq,
                                            Arc *arc) {
  KALDI_PARANOID_ASSERT(phone_seq[central_position_] != subsequential_symbol_);

  arc->ilabel = ilabel;
  arc->weight = Weight::One();
  arc->nextstate = dest;
  if (phone_seq[central_position_] == 0) {
    // This can happen at the beginning of the graph.  In this case we don't
    // output a real phone, we createdt an epsilon arc (but sometimes we need to
    // use a special disambiguation symbol instead of epsilon).
    arc->olabel = pseudo_eps_symbol_;
  } else {
    // We have a phone in the central position.
    arc->olabel = FindLabel(phone_seq);
  }
}

StdArc::StateId InverseContextFst::FindState(const vector<int32> &seq) {
  // Finds state-id corresponding to this vector of phones.  Inserts it if
  // necessary.
  KALDI_ASSERT(static_cast<int32>(seq.size()) == context_width_ - 1);
  VectorToStateMap::const_iterator iter = state_map_.find(seq);
  if (iter == state_map_.end()) {  // Not already in map.
    StateId this_state_id = (StateId)state_seqs_.size();
    state_seqs_.push_back(seq);
    state_map_[seq] = this_state_id;
    return this_state_id;
  } else {
    return iter->second;
  }
}

StdArc::Label InverseContextFst::FindLabel(const vector<int32> &label_vec) {
  // Finds the ilabel corresponding to this vector (creates a new ilabel if
  // necessary).
  VectorToLabelMap::const_iterator iter = ilabel_map_.find(label_vec);
  if (iter == ilabel_map_.end()) {  // Not already in map.
    Label this_label = ilabel_info_.size();
    ilabel_info_.push_back(label_vec);
    ilabel_map_[label_vec] = this_label;
    return this_label;
  } else {
    return iter->second;
  }
}


void ComposeContext(const vector<int32> &disambig_syms_in,
                    int32 context_width, int32 central_position,
                    VectorFst<StdArc> *ifst,
                    VectorFst<StdArc> *ofst,
                    vector<vector<int32> > *ilabels_out,
                    bool project_ifst) {
  KALDI_ASSERT(ifst != NULL && ofst != NULL);
  KALDI_ASSERT(context_width > 0);
  KALDI_ASSERT(central_position >= 0);
  KALDI_ASSERT(central_position < context_width);

  vector<int32> disambig_syms(disambig_syms_in);
  std::sort(disambig_syms.begin(), disambig_syms.end());

  vector<int32> all_syms;
  GetInputSymbols(*ifst, false/*no eps*/, &all_syms);
  std::sort(all_syms.begin(), all_syms.end());
  vector<int32> phones;
  for (size_t i = 0; i < all_syms.size(); i++)
    if (!std::binary_search(disambig_syms.begin(),
                            disambig_syms.end(), all_syms[i]))
      phones.push_back(all_syms[i]);

  // Get subsequential symbol that does not clash with
  // any disambiguation symbol or symbol in the FST.
  int32 subseq_sym = 1;
  if (!all_syms.empty())
    subseq_sym = std::max(subseq_sym, all_syms.back() + 1);
  if (!disambig_syms.empty())
    subseq_sym = std::max(subseq_sym, disambig_syms.back() + 1);

  // if central_position == context_width-1, it's left-context, and no
  // subsequential symbol is needed.
  if (central_position != context_width-1) {
    AddSubsequentialLoop(subseq_sym, ifst);
    if (project_ifst) {
      fst::Project(ifst, fst::PROJECT_INPUT);
    }
  }

  InverseContextFst inv_c(subseq_sym, phones, disambig_syms,
                          context_width, central_position);

  // The following statement is equivalent to the following
  // (if FSTs had the '*' operator for composition):
  //   (*ofst) = inv(inv_c) * (*ifst)
  ComposeDeterministicOnDemandInverse(*ifst, &inv_c, ofst);

  *ilabels_out = inv_c.IlabelInfo();
}

}  // end namespace fst
