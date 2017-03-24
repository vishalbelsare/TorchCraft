/**
 * Copyright (c) 2015-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "replayer.h"
#include <bitset>

namespace replayer = torchcraft::replayer;

// Serialization

std::ostream& replayer::operator<<(
    std::ostream& out,
    const replayer::Replayer& o) {
  auto width = THByteTensor_size(o.map.data, 0);
  auto height = THByteTensor_size(o.map.data, 1);
  auto data = THByteTensor_data(o.map.data);

  if (o.keyframe != 0) out << 0 << " " << o.keyframe << " ";
  out << width << " " << height << " ";
  out.write((const char *)data, height * width); // Write map data as raw bytes

  auto kf = o.keyframe == 0 ? 1 : o.keyframe;
  out << o.frames.size() << " ";
  for (size_t i = 0; i < o.frames.size(); i++) {
    if (i % kf == 0) out << *o.frames[i] << " ";
    else out << replayer::frame_diff(o.frames[i], o.frames[i - 1]) << " ";
  }

  out << o.numUnits.size() << " ";
  for (const auto& nu : o.numUnits) {
    out << nu.first << " " << nu.second << " ";
  }

  return out;
}

std::istream& replayer::operator>>(std::istream& in, replayer::Replayer& o) {
  // WARNING: cases were observed where this operator left a Replayer
  // that was in a corrupted state, and would produce a segfault
  // if we tried to delete it.
  // Cause: invalid data file? I/O error? or a bug in the code?

  int32_t diffed;
  int32_t width, height;
  in >> diffed;

  if (diffed == 0) in >> o.keyframe >> width >> height;
  else {
    width = diffed;
    in >> height;
    o.keyframe = 0;
  }
  diffed = (diffed == 0); // Every kf is a Frame, others are frame diffs
  if (height <= 0 || width <= 0)
    throw std::runtime_error("Corrupted replay: invalid map size");
  uint8_t* data = (uint8_t*)THAlloc(sizeof(uint8_t) * height * width);
  in.ignore(1); // Ignores next space
  in.read((char*)data, height * width); // Read some raw bytes
  o.setRawMap(width, height, data);
  size_t nFrames;
  in >> nFrames;
  o.frames.resize(nFrames);
  for (size_t i = 0; i < nFrames; i++) {
    if (o.keyframe == 0) {
      o.frames[i] = new Frame();
      in >> *o.frames[i];
    }
    else {
      if (i % o.keyframe == 0) {
        o.frames[i] = new Frame();
        in >> *o.frames[i];
      }
      else {
        replayer::FrameDiff du;
        in >> du;
        o.frames[i] = replayer::frame_undiff(&du, o.frames[i-1]);
      }
    }
  }

  int s;
  in >> s;
  if (s < 0)
    throw std::runtime_error("Corrupted replay: s < 0");
  int32_t key, val;
  for (auto i = 0; i < s; i++) {
    in >> key >> val;
    o.numUnits[key] = val;
  }

  return in;
}

void replayer::Replayer::setMap(THByteTensor* walkability,
    THByteTensor* ground_height, THByteTensor* buildability,
    std::vector<int>& start_loc_x, std::vector<int>& start_loc_y) {
  walkability = THByteTensor_newContiguous(walkability);
  ground_height = THByteTensor_newContiguous(ground_height);
  buildability = THByteTensor_newContiguous(buildability);
  replayer::Replayer::setMap(
      THByteTensor_size(walkability, 0),
      THByteTensor_size(walkability, 1),
      THByteTensor_data(walkability),
      THByteTensor_data(ground_height),
      THByteTensor_data(buildability),
      start_loc_x, start_loc_y);
  THByteTensor_free(walkability);
  THByteTensor_free(ground_height);
  THByteTensor_free(buildability);
}

#define WALKABILITY_SHIFT 0
#define BUILDABILITY_SHIFT 1
#define HEIGHT_SHIFT 2
// height is 0-5, hence 3 bits
#define START_LOC_SHIFT 5


void replayer::Replayer::setMap(int32_t w, int32_t h,
    uint8_t* walkability, uint8_t* ground_height, uint8_t* buildability,
    std::vector<int>& start_loc_x, std::vector<int>& start_loc_y) {
  if (map.data != nullptr) {
    THByteTensor_free(map.data);
  }
  map.data = THByteTensor_newWithSize2d(w, h);
  // The data is sent over transposed
  for (int x = 0; x < w; x++) {
    for (int y = 0; y < h; y++) {
      uint8_t v_w = walkability[x * h + y] & 1;
      uint8_t v_b = buildability[x * h + y] & 1;
      // Ground height only goes up to 5
      uint8_t v_g = ground_height[x * h + y] & 0b111;
      uint8_t packed = (v_w << WALKABILITY_SHIFT) |
        (v_b << BUILDABILITY_SHIFT) |
        (v_g << HEIGHT_SHIFT);
      THTensor_fastSet2d(map.data, x, y, packed);
    }
  }
  for (int i=0; i<start_loc_x.size(); i++) {
    auto x = start_loc_x[i];
    auto y = start_loc_y[i];
    auto v = THTensor_fastGet2d(map.data, x, y) | (1 << START_LOC_SHIFT);
    THTensor_fastSet2d(map.data, x, y, v);
  }
}

void replayer::Replayer::getMap(THByteTensor* walkability,
    THByteTensor* ground_height, THByteTensor* buildability,
    std::vector<int>& start_loc_x, std::vector<int>& start_loc_y) {
  auto w = THByteTensor_size(map.data, 0);
  auto h = THByteTensor_size(map.data, 1);
  THByteTensor_resizeAs(walkability, map.data);
  THByteTensor_resizeAs(ground_height, map.data);
  THByteTensor_resizeAs(buildability, map.data);
  start_loc_x.clear();
  start_loc_y.clear();
  for (int x = 0; x < w; x++) {
    for (int y = 0; y < h; y++) {
      uint8_t v = THTensor_fastGet2d(map.data, x, y);
      THTensor_fastSet2d(walkability, x, y, (v >> WALKABILITY_SHIFT) & 1);
      THTensor_fastSet2d(buildability, x, y, (v >> BUILDABILITY_SHIFT) & 1);
      THTensor_fastSet2d(ground_height, x, y, (v >> HEIGHT_SHIFT) & 0b111);
      bool is_start = ((v >> START_LOC_SHIFT) & 1) == 1;
      if (is_start) {
        start_loc_x.push_back(x);
        start_loc_y.push_back(y);
      }
    }
  }
}
