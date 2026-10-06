#include "dslr/lock_word.h"

namespace dslr {

std::string to_string(const LockWord& word) {
  return "{nX=" + std::to_string(word.nX) + ", nS=" + std::to_string(word.nS) +
         ", maxX=" + std::to_string(word.maxX) + ", maxS=" + std::to_string(word.maxS) + "}";
}

}  // namespace dslr
