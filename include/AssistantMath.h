#pragma once
// AssistantMath.h — pure evaluation maths shared by the HUD, the automation
// bridge and the unit tests (no graphics dependencies).

#include <cmath>
#include <cstdio>
#include <string>

#include "piece.hpp"

namespace assistant {

// WinProb = 1 / (1 + 10^(-eval / 4)), with eval in pawns (centipawns / 100).
inline double win_probability(int centipawns) {
  const double pawns{static_cast<double>(centipawns) / 100.0};
  return 1.0 / (1.0 + std::pow(10.0, -pawns / 4.0));
}

// White-perspective score -> probability that White wins. A forced mate is
// treated as (almost) certain.
inline double white_win_probability(int cp, bool is_mate, int mate_in) {
  if (is_mate) {
    return mate_in > 0 ? 1.0 : 0.0;
  }
  return win_probability(cp);
}

inline double user_win_probability(int cp, bool is_mate, int mate_in,
                                   PieceColor user) {
  const double white{white_win_probability(cp, is_mate, mate_in)};
  return user == PieceColor::Black ? 1.0 - white : white;
}

// "+0.34", "-1.50", "#3", "#-2" (always from White's perspective, as in most
// chess GUIs).
inline std::string format_eval(int cp, bool is_mate, int mate_in) {
  char buf[32];
  if (is_mate) {
    std::snprintf(buf, sizeof(buf), "#%d", mate_in);
  } else {
    std::snprintf(buf, sizeof(buf), "%+.2f", static_cast<double>(cp) / 100.0);
  }
  return buf;
}

}  // namespace assistant
