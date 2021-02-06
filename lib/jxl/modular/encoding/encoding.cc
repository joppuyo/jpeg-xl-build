// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lib/jxl/modular/encoding/encoding.h"

#include <stdint.h>
#include <stdlib.h>

#include <cinttypes>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "lib/jxl/base/status.h"
#include "lib/jxl/common.h"
#include "lib/jxl/dec_ans.h"
#include "lib/jxl/dec_bit_reader.h"
#include "lib/jxl/enc_ans.h"
#include "lib/jxl/enc_bit_writer.h"
#include "lib/jxl/entropy_coder.h"
#include "lib/jxl/fields.h"
#include "lib/jxl/image_ops.h"
#include "lib/jxl/modular/encoding/context_predict.h"
#include "lib/jxl/modular/encoding/ma.h"
#include "lib/jxl/modular/options.h"
#include "lib/jxl/modular/transform/transform.h"
#include "lib/jxl/toc.h"

namespace jxl {

namespace {
constexpr int32_t kWPPropRange = 512;
// Plot tree (if enabled) and predictor usage map.
constexpr bool kWantDebug = false;

// Removes all nodes that use a static property (i.e. channel or group ID) from
// the tree and collapses each node on even levels with its two children to
// produce a flatter tree. Also computes whether the resulting tree requires
// using the weighted predictor.
FlatTree FilterTree(const Tree &global_tree,
                    std::array<pixel_type, kNumStaticProperties> &static_props,
                    size_t *num_props, bool *use_wp, bool *wp_only) {
  *num_props = 0;
  bool has_wp = false;
  bool has_non_wp = false;
  constexpr size_t kMaxProp = 256;
  const auto mark_property = [&](int32_t p) {
    if (p == kWPProp) {
      has_wp = true;
    } else if (p >= kNumStaticProperties) {
      has_non_wp = true;
    }
  };
  FlatTree output;
  std::queue<size_t> nodes;
  nodes.push(0);
  // Produces a trimmed and flattened tree by doing a BFS visit of the original
  // tree, ignoring branches that are known to be false and proceeding two
  // levels at a time to collapse nodes in a flatter tree; if an inner parent
  // node has a leaf as a child, the leaf is duplicated and an implicit fake
  // node is added. This allows to reduce the number of branches when traversing
  // the resulting flat tree.
  while (!nodes.empty()) {
    size_t cur = nodes.front();
    nodes.pop();
    // Skip nodes that we can decide now, by jumping directly to their children.
    while (global_tree[cur].property < kNumStaticProperties &&
           global_tree[cur].property != -1) {
      if (static_props[global_tree[cur].property] > global_tree[cur].splitval) {
        cur = global_tree[cur].lchild;
      } else {
        cur = global_tree[cur].rchild;
      }
    }
    FlatDecisionNode flat;
    if (global_tree[cur].property == -1) {
      flat.property0 = -1;
      flat.childID = global_tree[cur].lchild;
      flat.predictor = global_tree[cur].predictor;
      flat.predictor_offset = global_tree[cur].predictor_offset;
      flat.multiplier = global_tree[cur].multiplier;
      mark_property(flat.predictor == Predictor::Weighted ? kWPProp : kMaxProp);
      output.push_back(flat);
      continue;
    }
    flat.childID = output.size() + nodes.size() + 1;

    flat.property0 = global_tree[cur].property;
    *num_props = std::max<size_t>(flat.property0 + 1, *num_props);
    flat.splitval0 = global_tree[cur].splitval;

    for (size_t i = 0; i < 2; i++) {
      size_t cur_child =
          i == 0 ? global_tree[cur].lchild : global_tree[cur].rchild;
      // Skip nodes that we can decide now.
      while (global_tree[cur_child].property < kNumStaticProperties &&
             global_tree[cur_child].property != -1) {
        if (static_props[global_tree[cur_child].property] >
            global_tree[cur_child].splitval) {
          cur_child = global_tree[cur_child].lchild;
        } else {
          cur_child = global_tree[cur_child].rchild;
        }
      }
      // We ended up in a leaf, add a dummy decision and two copies of the leaf.
      if (global_tree[cur_child].property == -1) {
        flat.properties[i] = 0;
        flat.splitvals[i] = 0;
        nodes.push(cur_child);
        nodes.push(cur_child);
      } else {
        flat.properties[i] = global_tree[cur_child].property;
        flat.splitvals[i] = global_tree[cur_child].splitval;
        nodes.push(global_tree[cur_child].lchild);
        nodes.push(global_tree[cur_child].rchild);
        *num_props = std::max<size_t>(flat.properties[i] + 1, *num_props);
      }
    }

    for (size_t j = 0; j < 2; j++) mark_property(flat.properties[j]);
    mark_property(flat.property0);
    output.push_back(flat);
  }
  if (*num_props > kNumNonrefProperties) {
    *num_props =
        DivCeil(*num_props - kNumNonrefProperties, kExtraPropsPerChannel) *
            kExtraPropsPerChannel +
        kNumNonrefProperties;
  } else {
    *num_props = kNumNonrefProperties;
  }
  *use_wp = has_wp;
  *wp_only = has_wp && !has_non_wp;

  return output;
}

}  // namespace

Status EncodeModularChannelMAANS(const Image &image, pixel_type chan,
                                 const weighted::Header &wp_header,
                                 const Tree &global_tree,
                                 std::vector<Token> *tokens, AuxOut *aux_out,
                                 size_t group_id) {
  const Channel &channel = image.channel[chan];

  JXL_ASSERT(channel.w != 0 && channel.h != 0);

  Image3F predictor_img;
  if (kWantDebug) predictor_img = Image3F(channel.w, channel.h);

  JXL_DEBUG_V(6,
              "Encoding %zux%zu channel %d, "
              "(shift=%i,%i, cshift=%i,%i)",
              channel.w, channel.h, chan, channel.hshift, channel.vshift,
              channel.hcshift, channel.vcshift);

  std::array<pixel_type, kNumStaticProperties> static_props = {chan,
                                                               (int)group_id};
  bool use_wp, is_wp_only;
  size_t num_props;
  FlatTree tree =
      FilterTree(global_tree, static_props, &num_props, &use_wp, &is_wp_only);
  Properties properties(num_props);
  MATreeLookup tree_lookup(tree);
  JXL_DEBUG_V(3, "Encoding using a MA tree with %zu nodes", tree.size());

  // Check if this tree is a WP-only tree with a small enough property value
  // range.
  // Initialized to avoid clang-tidy complaining.
  uint16_t context_lookup[2 * kWPPropRange] = {};
  // TODO(veluca): de-duplicate code in Decode.
  if (is_wp_only) {
    struct TreeRange {
      // Begin *excluded*, end *included*. This works best with > vs <= decision
      // nodes.
      int begin, end;
      size_t pos;
    };
    std::vector<TreeRange> ranges;
    ranges.push_back(TreeRange{-kWPPropRange - 1, kWPPropRange - 1, 0});
    while (!ranges.empty()) {
      TreeRange cur = ranges.back();
      ranges.pop_back();
      if (cur.begin < -kWPPropRange - 1 || cur.begin >= kWPPropRange - 1 ||
          cur.end > kWPPropRange - 1) {
        // Tree is outside the allowed range, exit.
        is_wp_only = false;
        break;
      }
      auto &node = tree[cur.pos];
      // Leaf.
      if (node.property0 == -1) {
        if (node.predictor_offset < std::numeric_limits<int8_t>::min() ||
            node.predictor_offset > std::numeric_limits<int8_t>::max() ||
            node.multiplier != 1 || node.predictor_offset != 0) {
          is_wp_only = false;
          break;
        }
        for (int i = cur.begin + 1; i < cur.end + 1; i++) {
          context_lookup[i + kWPPropRange] = node.childID;
        }
        continue;
      }
      // > side of top node.
      if (node.properties[0] >= kNumStaticProperties) {
        ranges.push_back(TreeRange({node.splitvals[0], cur.end, node.childID}));
        ranges.push_back(
            TreeRange({node.splitval0, node.splitvals[0], node.childID + 1}));
      } else {
        ranges.push_back(TreeRange({node.splitval0, cur.end, node.childID}));
      }
      // <= side
      if (node.properties[1] >= kNumStaticProperties) {
        ranges.push_back(
            TreeRange({node.splitvals[1], node.splitval0, node.childID + 2}));
        ranges.push_back(
            TreeRange({cur.begin, node.splitvals[1], node.childID + 3}));
      } else {
        ranges.push_back(
            TreeRange({cur.begin, node.splitval0, node.childID + 2}));
      }
    }
  }

  tokens->reserve(tokens->size() + channel.w * channel.h);
  if (is_wp_only) {
    for (size_t c = 0; c < 3; c++) {
      FillImage(static_cast<float>(PredictorColor(Predictor::Weighted)[c]),
                &predictor_img.Plane(c));
    }
    const intptr_t onerow = channel.plane.PixelsPerRow();
    weighted::State wp_state(wp_header, channel.w, channel.h);
    Properties properties(1);
    for (size_t y = 0; y < channel.h; y++) {
      const pixel_type *JXL_RESTRICT r = channel.Row(y);
      for (size_t x = 0; x < channel.w; x++) {
        size_t offset = 0;
        pixel_type_w left = (x ? r[x - 1] : y ? *(r + x - onerow) : 0);
        pixel_type_w top = (y ? *(r + x - onerow) : left);
        pixel_type_w topleft = (x && y ? *(r + x - 1 - onerow) : left);
        pixel_type_w topright =
            (x + 1 < channel.w && y ? *(r + x + 1 - onerow) : top);
        pixel_type_w toptop = (y > 1 ? *(r + x - onerow - onerow) : top);
        int32_t guess = wp_state.Predict</*compute_properties=*/true>(
            x, y, channel.w, top, left, topright, topleft, toptop, &properties,
            offset);
        uint32_t pos =
            kWPPropRange +
            std::min(std::max(-kWPPropRange, properties[0]), kWPPropRange - 1);
        uint32_t ctx_id = context_lookup[pos];
        int32_t residual = r[x] - guess;
        tokens->emplace_back(ctx_id, PackSigned(residual));
        wp_state.UpdateErrors(r[x], x, y, channel.w);
      }
    }
  } else if (tree.size() == 1 && tree[0].predictor == Predictor::Zero &&
             tree[0].multiplier == 1 && tree[0].predictor_offset == 0) {
    for (size_t c = 0; c < 3; c++) {
      FillImage(static_cast<float>(PredictorColor(Predictor::Zero)[c]),
                &predictor_img.Plane(c));
    }
    for (size_t y = 0; y < channel.h; y++) {
      const pixel_type *JXL_RESTRICT p = channel.Row(y);
      for (size_t x = 0; x < channel.w; x++) {
        tokens->emplace_back(tree[0].childID, PackSigned(p[x]));
      }
    }
  } else if (tree.size() == 1 && tree[0].predictor != Predictor::Weighted &&
             (tree[0].multiplier & (tree[0].multiplier - 1)) == 0 &&
             tree[0].predictor_offset == 0) {
    // multiplier is a power of 2.
    for (size_t c = 0; c < 3; c++) {
      FillImage(static_cast<float>(PredictorColor(tree[0].predictor)[c]),
                &predictor_img.Plane(c));
    }
    uint32_t mul_shift = FloorLog2Nonzero((uint32_t)tree[0].multiplier);
    const intptr_t onerow = channel.plane.PixelsPerRow();
    for (size_t y = 0; y < channel.h; y++) {
      const pixel_type *JXL_RESTRICT r = channel.Row(y);
      for (size_t x = 0; x < channel.w; x++) {
        PredictionResult pred = PredictNoTreeNoWP(channel.w, r + x, onerow, x,
                                                  y, tree[0].predictor);
        pixel_type_w residual = r[x] - pred.guess;
        JXL_DASSERT((residual >> mul_shift) * tree[0].multiplier == residual);
        tokens->emplace_back(tree[0].childID,
                             PackSigned(residual >> mul_shift));
      }
    }

  } else if (!use_wp) {
    const intptr_t onerow = channel.plane.PixelsPerRow();
    Channel references(properties.size() - kNumNonrefProperties, channel.w);
    for (size_t y = 0; y < channel.h; y++) {
      const pixel_type *JXL_RESTRICT p = channel.Row(y);
      PrecomputeReferences(channel, y, image, chan, &references);
      float *pred_img_row[3];
      if (kWantDebug) {
        for (size_t c = 0; c < 3; c++) {
          pred_img_row[c] = predictor_img.PlaneRow(c, y);
        }
      }
      InitPropsRow(&properties, static_props, y);
      for (size_t x = 0; x < channel.w; x++) {
        PredictionResult res =
            PredictTreeNoWP(&properties, channel.w, p + x, onerow, x, y,
                            tree_lookup, references);
        if (kWantDebug) {
          for (size_t i = 0; i < 3; i++) {
            pred_img_row[i][x] = PredictorColor(res.predictor)[i];
          }
        }
        pixel_type_w residual = p[x] - res.guess;
        JXL_ASSERT(residual % res.multiplier == 0);
        tokens->emplace_back(res.context,
                             PackSigned(residual / res.multiplier));
      }
    }
  } else {
    const intptr_t onerow = channel.plane.PixelsPerRow();
    Channel references(properties.size() - kNumNonrefProperties, channel.w);
    weighted::State wp_state(wp_header, channel.w, channel.h);
    for (size_t y = 0; y < channel.h; y++) {
      const pixel_type *JXL_RESTRICT p = channel.Row(y);
      PrecomputeReferences(channel, y, image, chan, &references);
      float *pred_img_row[3];
      if (kWantDebug) {
        for (size_t c = 0; c < 3; c++) {
          pred_img_row[c] = predictor_img.PlaneRow(c, y);
        }
      }
      InitPropsRow(&properties, static_props, y);
      for (size_t x = 0; x < channel.w; x++) {
        PredictionResult res =
            PredictTreeWP(&properties, channel.w, p + x, onerow, x, y,
                          tree_lookup, references, &wp_state);
        if (kWantDebug) {
          for (size_t i = 0; i < 3; i++) {
            pred_img_row[i][x] = PredictorColor(res.predictor)[i];
          }
        }
        pixel_type_w residual = p[x] - res.guess;
        JXL_ASSERT(residual % res.multiplier == 0);
        tokens->emplace_back(res.context,
                             PackSigned(residual / res.multiplier));
        wp_state.UpdateErrors(p[x], x, y, channel.w);
      }
    }
  }
  if (kWantDebug && WantDebugOutput(aux_out)) {
    aux_out->DumpImage(
        ("pred_" + std::to_string(group_id) + "_" + std::to_string(chan))
            .c_str(),
        predictor_img);
  }
  return true;
}

Status DecodeModularChannelMAANS(BitReader *br, ANSSymbolReader *reader,
                                 const std::vector<uint8_t> &context_map,
                                 const Tree &global_tree,
                                 const weighted::Header &wp_header,
                                 pixel_type chan, size_t group_id,
                                 Image *image) {
  Channel &channel = image->channel[chan];

  std::array<pixel_type, kNumStaticProperties> static_props = {chan,
                                                               (int)group_id};
  // TODO(veluca): filter the tree according to static_props.

  // zero pixel channel? could happen
  if (channel.w == 0 || channel.h == 0) return true;

  channel.resize(channel.w, channel.h);
  bool tree_has_wp_prop_or_pred = false;
  bool is_wp_only = false;
  size_t num_props;
  FlatTree tree = FilterTree(global_tree, static_props, &num_props,
                             &tree_has_wp_prop_or_pred, &is_wp_only);

  // From here on, tree lookup returns a *clustered* context ID.
  // This avoids an extra memory lookup after tree traversal.
  for (size_t i = 0; i < tree.size(); i++) {
    if (tree[i].property0 == -1) {
      tree[i].childID = context_map[tree[i].childID];
    }
  }

  JXL_DEBUG_V(3, "Decoded MA tree with %zu nodes", tree.size());

  // MAANS decode

  // Check if this tree is a WP-only tree with a small enough property value
  // range.
  // Those contexts are *clustered* context ids. This reduces stack usages and
  // avoids an extra memory lookup.
  // Initialized to avoid clang-tidy complaining.
  uint8_t context_lookup[2 * kWPPropRange] = {};
  int32_t multipliers[2 * kWPPropRange] = {};
  int8_t offsets[2 * kWPPropRange] = {};
  if (is_wp_only) {
    struct TreeRange {
      // Begin *excluded*, end *included*. This works best with > vs <= decision
      // nodes.
      int begin, end;
      size_t pos;
    };
    std::vector<TreeRange> ranges;
    ranges.push_back(TreeRange{-kWPPropRange - 1, kWPPropRange - 1, 0});
    while (!ranges.empty()) {
      TreeRange cur = ranges.back();
      ranges.pop_back();
      if (cur.begin < -kWPPropRange - 1 || cur.begin >= kWPPropRange - 1 ||
          cur.end > kWPPropRange - 1) {
        // Tree is outside the allowed range, exit.
        is_wp_only = false;
        break;
      }
      auto &node = tree[cur.pos];
      // Leaf.
      if (node.property0 == -1) {
        if (node.predictor_offset < std::numeric_limits<int8_t>::min() ||
            node.predictor_offset > std::numeric_limits<int8_t>::max()) {
          is_wp_only = false;
          break;
        }
        for (int i = cur.begin + 1; i < cur.end + 1; i++) {
          context_lookup[i + kWPPropRange] = node.childID;
          multipliers[i + kWPPropRange] = node.multiplier;
          offsets[i + kWPPropRange] = node.predictor_offset;
        }
        continue;
      }
      // > side of top node.
      if (node.properties[0] >= kNumStaticProperties) {
        ranges.push_back(TreeRange({node.splitvals[0], cur.end, node.childID}));
        ranges.push_back(
            TreeRange({node.splitval0, node.splitvals[0], node.childID + 1}));
      } else {
        ranges.push_back(TreeRange({node.splitval0, cur.end, node.childID}));
      }
      // <= side
      if (node.properties[1] >= kNumStaticProperties) {
        ranges.push_back(
            TreeRange({node.splitvals[1], node.splitval0, node.childID + 2}));
        ranges.push_back(
            TreeRange({cur.begin, node.splitvals[1], node.childID + 3}));
      } else {
        ranges.push_back(
            TreeRange({cur.begin, node.splitval0, node.childID + 2}));
      }
    }
  }

  if (is_wp_only) {
    JXL_DEBUG_V(8, "WP fast track.");
    const intptr_t onerow = channel.plane.PixelsPerRow();
    weighted::State wp_state(wp_header, channel.w, channel.h);
    Properties properties(1);
    for (size_t y = 0; y < channel.h; y++) {
      pixel_type *JXL_RESTRICT r = channel.Row(y);
      for (size_t x = 0; x < channel.w; x++) {
        size_t offset = 0;
        pixel_type_w left = (x ? r[x - 1] : y ? *(r + x - onerow) : 0);
        pixel_type_w top = (y ? *(r + x - onerow) : left);
        pixel_type_w topleft = (x && y ? *(r + x - 1 - onerow) : left);
        pixel_type_w topright =
            (x + 1 < channel.w && y ? *(r + x + 1 - onerow) : top);
        pixel_type_w toptop = (y > 1 ? *(r + x - onerow - onerow) : top);
        int32_t guess = wp_state.Predict</*compute_properties=*/true>(
            x, y, channel.w, top, left, topright, topleft, toptop, &properties,
            offset);
        uint32_t pos =
            kWPPropRange +
            std::min(std::max(-kWPPropRange, properties[0]), kWPPropRange - 1);
        uint32_t ctx_id = context_lookup[pos];
        uint64_t v = reader->ReadHybridUintClustered(ctx_id, br);
        r[x] = SaturatingAdd<pixel_type>(
            UnpackSigned(v) * multipliers[pos] + offsets[pos], guess);
        wp_state.UpdateErrors(r[x], x, y, channel.w);
      }
    }
  } else if (tree.size() == 1) {
    // special optimized case: no meta-adaptation, so no need
    // to compute properties.
    Predictor predictor = tree[0].predictor;
    int64_t offset = tree[0].predictor_offset;
    int32_t multiplier = tree[0].multiplier;
    size_t ctx_id = tree[0].childID;
    if (predictor == Predictor::Zero) {
      uint32_t value;
      if (reader->IsSingleValue(ctx_id, &value, channel.w * channel.h)) {
        // Special-case: histogram has a single symbol, with no extra bits, and
        // we use ANS mode.
        JXL_DEBUG_V(8, "Fastest track.");
        pixel_type v =
            SaturatingAdd<pixel_type>(UnpackSigned(value) * multiplier, offset);
        for (size_t y = 0; y < channel.h; y++) {
          pixel_type *JXL_RESTRICT r = channel.Row(y);
          std::fill(r, r + channel.w, v);
        }

      } else {
        JXL_DEBUG_V(8, "Fast track.");
        for (size_t y = 0; y < channel.h; y++) {
          pixel_type *JXL_RESTRICT r = channel.Row(y);
          for (size_t x = 0; x < channel.w; x++) {
            uint32_t v = reader->ReadHybridUintClustered(ctx_id, br);
            r[x] =
                SaturatingAdd<pixel_type>(UnpackSigned(v) * multiplier, offset);
          }
        }
      }
    } else if (predictor != Predictor::Weighted) {
      // special optimized case: no meta-adaptation, no wp, so no need to
      // compute properties
      JXL_DEBUG_V(8, "Quite fast track.");
      const intptr_t onerow = channel.plane.PixelsPerRow();
      for (size_t y = 0; y < channel.h; y++) {
        pixel_type *JXL_RESTRICT r = channel.Row(y);
        for (size_t x = 0; x < channel.w; x++) {
          PredictionResult pred =
              PredictNoTreeNoWP(channel.w, r + x, onerow, x, y, predictor);
          pixel_type_w g = pred.guess + offset;
          uint64_t v = reader->ReadHybridUintClustered(ctx_id, br);
          // NOTE: pred.multiplier is unset.
          r[x] = SaturatingAdd<pixel_type>(UnpackSigned(v) * multiplier, g);
        }
      }
    } else {
      // special optimized case: no meta-adaptation, so no need to
      // compute properties
      JXL_DEBUG_V(8, "Somewhat fast track.");
      const intptr_t onerow = channel.plane.PixelsPerRow();
      weighted::State wp_state(wp_header, channel.w, channel.h);
      for (size_t y = 0; y < channel.h; y++) {
        pixel_type *JXL_RESTRICT r = channel.Row(y);
        for (size_t x = 0; x < channel.w; x++) {
          pixel_type_w g = PredictNoTreeWP(channel.w, r + x, onerow, x, y,
                                           predictor, &wp_state)
                               .guess +
                           offset;
          uint64_t v = reader->ReadHybridUintClustered(ctx_id, br);
          r[x] = SaturatingAdd<pixel_type>(UnpackSigned(v) * multiplier, g);
          wp_state.UpdateErrors(r[x], x, y, channel.w);
        }
      }
    }
  } else if (!tree_has_wp_prop_or_pred) {
    // special optimized case: the weighted predictor and its properties are not
    // used, so no need to compute weights and properties.
    JXL_DEBUG_V(8, "Slow track.");
    MATreeLookup tree_lookup(tree);
    Properties properties = Properties(num_props);
    const intptr_t onerow = channel.plane.PixelsPerRow();
    Channel references(properties.size() - kNumNonrefProperties, channel.w);
    for (size_t y = 0; y < channel.h; y++) {
      pixel_type *JXL_RESTRICT p = channel.Row(y);
      PrecomputeReferences(channel, y, *image, chan, &references);
      InitPropsRow(&properties, static_props, y);
      for (size_t x = 0; x < channel.w; x++) {
        PredictionResult res =
            PredictTreeNoWP(&properties, channel.w, p + x, onerow, x, y,
                            tree_lookup, references);
        uint64_t v = reader->ReadHybridUintClustered(res.context, br);
        p[x] = SaturatingAdd<pixel_type>(UnpackSigned(v) * res.multiplier,
                                         res.guess);
      }
    }
  } else {
    JXL_DEBUG_V(8, "Slowest track.");
    MATreeLookup tree_lookup(tree);
    Properties properties = Properties(num_props);
    const intptr_t onerow = channel.plane.PixelsPerRow();
    Channel references(properties.size() - kNumNonrefProperties, channel.w);
    weighted::State wp_state(wp_header, channel.w, channel.h);
    for (size_t y = 0; y < channel.h; y++) {
      pixel_type *JXL_RESTRICT p = channel.Row(y);
      InitPropsRow(&properties, static_props, y);
      PrecomputeReferences(channel, y, *image, chan, &references);
      for (size_t x = 0; x < channel.w; x++) {
        PredictionResult res =
            PredictTreeWP(&properties, channel.w, p + x, onerow, x, y,
                          tree_lookup, references, &wp_state);
        uint64_t v = reader->ReadHybridUintClustered(res.context, br);
        p[x] = SaturatingAdd<pixel_type>(UnpackSigned(v) * res.multiplier,
                                         res.guess);
        wp_state.UpdateErrors(p[x], x, y, channel.w);
      }
    }
  }
  return true;
}

void GatherTreeData(const Image &image, pixel_type chan, size_t group_id,
                    const weighted::Header &wp_header,
                    const ModularOptions &options, TreeSamples &tree_samples,
                    size_t *total_pixels) {
  const Channel &channel = image.channel[chan];

  JXL_DEBUG_V(7, "Learning %zux%zu channel %d", channel.w, channel.h, chan);

  std::array<pixel_type, kNumStaticProperties> static_props = {chan,
                                                               (int)group_id};
  Properties properties(kNumNonrefProperties +
                        kExtraPropsPerChannel * options.max_properties);
  double pixel_fraction = std::min(1.0f, options.nb_repeats);
  // a fraction of 0 is used to disable learning entirely.
  if (pixel_fraction > 0) {
    pixel_fraction = std::max(pixel_fraction,
                              std::min(1.0, 1024.0 / (channel.w * channel.h)));
  }
  uint64_t threshold =
      (std::numeric_limits<uint64_t>::max() >> 32) * pixel_fraction;
  uint64_t s[2] = {0x94D049BB133111EBull, 0xBF58476D1CE4E5B9ull};
  // Xorshift128+ adapted from xorshift128+-inl.h
  auto use_sample = [&]() {
    auto s1 = s[0];
    const auto s0 = s[1];
    const auto bits = s1 + s0;  // b, c
    s[0] = s0;
    s1 ^= s1 << 23;
    s1 ^= s0 ^ (s1 >> 18) ^ (s0 >> 5);
    s[1] = s1;
    return (bits >> 32) <= threshold;
  };

  const intptr_t onerow = channel.plane.PixelsPerRow();
  Channel references(properties.size() - kNumNonrefProperties, channel.w);
  weighted::State wp_state(wp_header, channel.w, channel.h);
  tree_samples.PrepareForSamples(pixel_fraction * channel.h * channel.w + 64);
  for (size_t y = 0; y < channel.h; y++) {
    const pixel_type *JXL_RESTRICT p = channel.Row(y);
    PrecomputeReferences(channel, y, image, chan, &references);
    InitPropsRow(&properties, static_props, y);
    // TODO(veluca): avoid computing WP if we don't use its property or
    // predictions.
    for (size_t x = 0; x < channel.w; x++) {
      pixel_type_w pred[kNumModularPredictors];
      if (tree_samples.NumPredictors() != 1) {
        PredictLearnAll(&properties, channel.w, p + x, onerow, x, y, references,
                        &wp_state, pred);
      } else {
        pred[static_cast<int>(tree_samples.PredictorFromIndex(0))] =
            PredictLearn(&properties, channel.w, p + x, onerow, x, y,
                         tree_samples.PredictorFromIndex(0), references,
                         &wp_state)
                .guess;
      }
      (*total_pixels)++;
      if (use_sample()) {
        tree_samples.AddSample(p[x], properties, pred);
      }
      wp_state.UpdateErrors(p[x], x, y, channel.w);
    }
  }
}

Tree LearnTree(TreeSamples &&tree_samples, size_t total_pixels,
               const ModularOptions &options,
               const std::vector<ModularMultiplierInfo> &multiplier_info,
               StaticPropRange static_prop_range) {
  for (size_t i = 0; i < kNumStaticProperties; i++) {
    if (static_prop_range[i][1] == 0) {
      static_prop_range[i][1] = std::numeric_limits<uint32_t>::max();
    }
  }
  if (!tree_samples.HasSamples()) {
    Tree tree;
    tree.emplace_back();
    tree.back().predictor = tree_samples.PredictorFromIndex(0);
    tree.back().property = -1;
    tree.back().predictor_offset = 0;
    tree.back().multiplier = 1;
    return tree;
  }
  float pixel_fraction = tree_samples.NumSamples() * 1.0f / total_pixels;
  float required_cost = pixel_fraction * 0.9 + 0.1;
  tree_samples.AllSamplesDone();
  Tree tree;
  ComputeBestTree(tree_samples,
                  options.splitting_heuristics_node_threshold * required_cost,
                  multiplier_info, static_prop_range,
                  options.fast_decode_multiplier, &tree);
  return tree;
}

GroupHeader::GroupHeader() { Bundle::Init(this); }

constexpr bool kPrintTree = false;

void PrintTree(const Tree &tree, const std::string &path) {
  if (!kPrintTree) return;
  FILE *f = fopen((path + ".dot").c_str(), "w");
  fprintf(f, "graph{\n");
  for (size_t cur = 0; cur < tree.size(); cur++) {
    if (tree[cur].property < 0) {
      fprintf(f, "n%05zu [label=\"%s%+" PRId64 " (x%u)\"];\n", cur,
              PredictorName(tree[cur].predictor), tree[cur].predictor_offset,
              tree[cur].multiplier);
    } else {
      fprintf(f, "n%05zu [label=\"%s>%d\"];\n", cur,
              PropertyName(tree[cur].property).c_str(), tree[cur].splitval);
      fprintf(f, "n%05zu -- n%05d;\n", cur, tree[cur].lchild);
      fprintf(f, "n%05zu -- n%05d;\n", cur, tree[cur].rchild);
    }
  }
  fprintf(f, "}\n");
  fclose(f);
  JXL_ASSERT(
      system(("dot " + path + ".dot -T svg -o " + path + ".svg").c_str()) == 0);
}

Status ModularEncode(const Image &image, const ModularOptions &options,
                     BitWriter *writer, AuxOut *aux_out, size_t layer,
                     size_t group_id, TreeSamples *tree_samples,
                     size_t *total_pixels, const Tree *tree,
                     GroupHeader *header, std::vector<Token> *tokens,
                     size_t *width) {
  if (image.error) return JXL_FAILURE("Invalid image");
  size_t nb_channels = image.channel.size();
  int bit_depth = 1, maxval = 1;
  while (maxval < image.maxval) {
    bit_depth++;
    maxval = maxval * 2 + 1;
  }
  JXL_DEBUG_V(2, "Encoding %zu-channel, %i-bit, %zux%zu image.", nb_channels,
              bit_depth, image.w, image.h);

  if (nb_channels < 1) {
    return true;  // is there any use for a zero-channel image?
  }

  // encode transforms
  GroupHeader header_storage;
  if (header == nullptr) header = &header_storage;
  Bundle::Init(header);
  if (options.predictor == Predictor::Weighted) {
    weighted::PredictorMode(options.wp_mode, &header->wp_header);
  }
  header->transforms = image.transform;
  // This doesn't actually work
  if (tree != nullptr) {
    header->use_global_tree = true;
  }
  if (tree_samples == nullptr && tree == nullptr) {
    JXL_RETURN_IF_ERROR(Bundle::Write(*header, writer, layer, aux_out));
  }

  TreeSamples tree_samples_storage;
  size_t total_pixels_storage = 0;
  if (!total_pixels) total_pixels = &total_pixels_storage;
  // If there's no tree, compute one (or gather data to).
  if (tree == nullptr) {
    bool gather_data = tree_samples != nullptr;
    if (tree_samples == nullptr) {
      JXL_RETURN_IF_ERROR(tree_samples_storage.SetPredictor(
          options.predictor, options.wp_tree_mode));
      JXL_RETURN_IF_ERROR(tree_samples_storage.SetProperties(
          options.splitting_heuristics_properties, options.wp_tree_mode));
      std::vector<pixel_type> pixel_samples;
      std::vector<pixel_type> diff_samples;
      std::vector<uint32_t> group_pixel_count;
      std::vector<uint32_t> channel_pixel_count;
      CollectPixelSamples(image, options, 0, group_pixel_count,
                          channel_pixel_count, pixel_samples, diff_samples);
      std::vector<ModularMultiplierInfo> dummy_multiplier_info;
      StaticPropRange range;
      tree_samples_storage.PreQuantizeProperties(
          range, dummy_multiplier_info, group_pixel_count, channel_pixel_count,
          pixel_samples, diff_samples, options.max_property_values);
    }
    for (size_t i = options.skipchannels; i < nb_channels; i++) {
      if (!image.channel[i].w || !image.channel[i].h) {
        continue;  // skip empty channels
      }
      if (i >= image.nb_meta_channels &&
          (image.channel[i].w > options.max_chan_size ||
           image.channel[i].h > options.max_chan_size)) {
        break;
      }
      GatherTreeData(image, i, group_id, header->wp_header, options,
                     gather_data ? *tree_samples : tree_samples_storage,
                     total_pixels);
    }
    if (gather_data) return true;
  }

  JXL_ASSERT((tree == nullptr) == (tokens == nullptr));

  Tree tree_storage;
  std::vector<std::vector<Token>> tokens_storage(1);
  // Compute tree.
  if (tree == nullptr) {
    EntropyEncodingData code;
    std::vector<uint8_t> context_map;

    std::vector<std::vector<Token>> tree_tokens(1);
    tree_storage =
        LearnTree(std::move(tree_samples_storage), *total_pixels, options);
    tree = &tree_storage;
    tokens = &tokens_storage[0];

    Tree decoded_tree;
    TokenizeTree(*tree, &tree_tokens[0], &decoded_tree);
    JXL_ASSERT(tree->size() == decoded_tree.size());
    tree_storage = std::move(decoded_tree);

    if (kWantDebug && WantDebugOutput(aux_out)) {
      PrintTree(*tree,
                aux_out->debug_prefix + "/tree_" + std::to_string(group_id));
    }
    // Write tree
    BuildAndEncodeHistograms(HistogramParams(), kNumTreeContexts, tree_tokens,
                             &code, &context_map, writer, kLayerModularTree,
                             aux_out);
    WriteTokens(tree_tokens[0], code, context_map, writer, kLayerModularTree,
                aux_out);
  }

  size_t image_width = 0;
  for (size_t i = options.skipchannels; i < nb_channels; i++) {
    if (!image.channel[i].w || !image.channel[i].h) {
      continue;  // skip empty channels
    }
    if (i >= image.nb_meta_channels &&
        (image.channel[i].w > options.max_chan_size ||
         image.channel[i].h > options.max_chan_size)) {
      break;
    }
    if (image.channel[i].w > image_width) image_width = image.channel[i].w;
    JXL_RETURN_IF_ERROR(EncodeModularChannelMAANS(
        image, i, header->wp_header, *tree, tokens, aux_out, group_id));
  }

  // Write data if not using a global tree/ANS stream.
  if (!header->use_global_tree) {
    EntropyEncodingData code;
    std::vector<uint8_t> context_map;
    HistogramParams histo_params;
    histo_params.image_widths.push_back(image_width);
    BuildAndEncodeHistograms(histo_params, (tree->size() + 1) / 2,
                             tokens_storage, &code, &context_map, writer, layer,
                             aux_out);
    WriteTokens(tokens_storage[0], code, context_map, writer, layer, aux_out);
  } else {
    *width = image_width;
  }
  return true;
}

Status ModularDecode(BitReader *br, Image &image, GroupHeader &header,
                     size_t group_id, ModularOptions *options,
                     const Tree *global_tree, const ANSCode *global_code,
                     const std::vector<uint8_t> *global_ctx_map,
                     bool allow_truncated_group) {
  if (image.nb_channels < 1) return true;

  // decode transforms
  JXL_RETURN_IF_ERROR(Bundle::Read(br, &header));
  JXL_DEBUG_V(4, "Global option: up to %i back-referencing MA properties.",
              options->max_properties);
  JXL_DEBUG_V(3, "Image data underwent %zu transformations: ",
              header.transforms.size());
  image.transform = header.transforms;
  for (Transform &transform : image.transform) {
    JXL_RETURN_IF_ERROR(transform.MetaApply(image));
  }
  if (options->identify) return true;
  if (image.error) {
    return JXL_FAILURE("Corrupt file. Aborting.");
  }

  size_t nb_channels = image.channel.size();

  size_t num_chans = 0;
  for (size_t i = options->skipchannels; i < nb_channels; i++) {
    if (!image.channel[i].w || !image.channel[i].h) {
      continue;  // skip empty channels
    }
    if (i >= image.nb_meta_channels &&
        (image.channel[i].w > options->max_chan_size ||
         image.channel[i].h > options->max_chan_size)) {
      break;
    }
    num_chans++;
  }
  if (num_chans == 0) return true;

  // Read tree.
  Tree tree_storage;
  std::vector<uint8_t> context_map_storage;
  ANSCode code_storage;
  const Tree *tree = &tree_storage;
  const ANSCode *code = &code_storage;
  const std::vector<uint8_t> *context_map = &context_map_storage;
  if (!header.use_global_tree) {
    size_t tree_size_limit = 1024 + image.w * image.h * nb_channels;
    JXL_RETURN_IF_ERROR(DecodeTree(br, &tree_storage, tree_size_limit));
    JXL_RETURN_IF_ERROR(DecodeHistograms(br, (tree_storage.size() + 1) / 2,
                                         &code_storage, &context_map_storage));
  } else {
    if (!global_tree || !global_code || !global_ctx_map ||
        global_tree->empty()) {
      return JXL_FAILURE("No global tree available but one was requested");
    }
    tree = global_tree;
    code = global_code;
    context_map = global_ctx_map;
  }

  size_t distance_multiplier = 0;
  for (size_t i = options->skipchannels; i < nb_channels; i++) {
    Channel &channel = image.channel[i];
    if (!channel.w || !channel.h) {
      continue;  // skip empty channels
    }
    if (i >= image.nb_meta_channels && (channel.w > options->max_chan_size ||
                                        channel.h > options->max_chan_size)) {
      break;
    }
    if (channel.w > distance_multiplier) {
      distance_multiplier = channel.w;
    }
  }
  // Read channels
  ANSSymbolReader reader(code, br, distance_multiplier);
  for (size_t i = options->skipchannels; i < nb_channels; i++) {
    Channel &channel = image.channel[i];
    if (!channel.w || !channel.h) {
      continue;  // skip empty channels
    }
    if (i >= image.nb_meta_channels && (channel.w > options->max_chan_size ||
                                        channel.h > options->max_chan_size)) {
      break;
    }
    JXL_RETURN_IF_ERROR(DecodeModularChannelMAANS(br, &reader, *context_map,
                                                  *tree, header.wp_header, i,
                                                  group_id, &image));
    // Truncated group.
    if (allow_truncated_group && !br->AllReadsWithinBounds()) {
      ZeroFillImage(&channel.plane);
      return Status(StatusCode::kNotEnoughBytes);
    }
  }
  if (!reader.CheckANSFinalState()) {
    return JXL_FAILURE("ANS decode final state failed");
  }
  return true;
}

Status ModularGenericCompress(Image &image, const ModularOptions &opts,
                              BitWriter *writer, AuxOut *aux_out, size_t layer,
                              size_t group_id, TreeSamples *tree_samples,
                              size_t *total_pixels, const Tree *tree,
                              GroupHeader *header, std::vector<Token> *tokens,
                              size_t *width) {
  if (image.w == 0 || image.h == 0) return true;
  ModularOptions options = opts;  // Make a copy to modify it.

  if (options.predictor == static_cast<Predictor>(-1)) {
    options.predictor = Predictor::Gradient;
  }

  size_t bits = writer ? writer->BitsWritten() : 0;
  JXL_RETURN_IF_ERROR(ModularEncode(image, options, writer, aux_out, layer,
                                    group_id, tree_samples, total_pixels, tree,
                                    header, tokens, width));
  bits = writer ? writer->BitsWritten() - bits : 0;
  if (writer) {
    JXL_DEBUG_V(
        4, "Modular-encoded a %zux%zu maxval=%i nbchans=%zu image in %zu bytes",
        image.w, image.h, image.maxval, image.real_nb_channels, bits / 8);
  }
  (void)bits;
  return true;
}

Status ModularGenericDecompress(BitReader *br, Image &image,
                                GroupHeader *header, size_t group_id,
                                ModularOptions *options, int undo_transforms,
                                const Tree *tree, const ANSCode *code,
                                const std::vector<uint8_t> *ctx_map,
                                bool allow_truncated_group) {
#ifdef JXL_ENABLE_ASSERT
  std::vector<std::pair<uint32_t, uint32_t>> req_sizes(image.channel.size());
  for (size_t c = 0; c < req_sizes.size(); c++) {
    req_sizes[c] = {image.channel[c].w, image.channel[c].h};
  }
#endif
  GroupHeader local_header;
  if (header == nullptr) header = &local_header;
  auto dec_status = ModularDecode(br, image, *header, group_id, options, tree,
                                  code, ctx_map, allow_truncated_group);
  if (dec_status.IsFatalError()) return dec_status;
  image.undo_transforms(header->wp_header, undo_transforms);
  if (image.error) return JXL_FAILURE("Corrupt file. Aborting.");
  size_t bit_pos = br->TotalBitsConsumed();
  JXL_DEBUG_V(4, "Modular-decoded a %zux%zu nbchans=%zu image from %zu bytes",
              image.w, image.h, image.real_nb_channels,
              (br->TotalBitsConsumed() - bit_pos) / 8);
  (void)bit_pos;
#ifdef JXL_ENABLE_ASSERT
  // Check that after applying all transforms we are back to the requested image
  // sizes, otherwise there's a programming error with the transformations.
  if (undo_transforms == -1 || undo_transforms == 0) {
    JXL_ASSERT(image.channel.size() == req_sizes.size());
    for (size_t c = 0; c < req_sizes.size(); c++) {
      JXL_ASSERT(req_sizes[c].first == image.channel[c].w);
      JXL_ASSERT(req_sizes[c].second == image.channel[c].h);
    }
  }
#endif
  return dec_status;
}

}  // namespace jxl
