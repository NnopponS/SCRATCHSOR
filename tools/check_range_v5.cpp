#include <iostream>
#include <iomanip>
#include "product/range_v5.h"
// Read a median feature vector followed by its window count and window vectors.
int main() {
  float f[16], windows[16][16]; unsigned count;
  while (std::cin >> f[0]) {
    for (unsigned i=1;i<16;i++) std::cin >> f[i];
    std::cin >> count;
    if (count>16) return 2;
    for (unsigned j=0;j<count;j++) for (float& v:windows[j]) std::cin >> v;
    if (!std::cin) return 3;
    auto p=count?range_model::capture(f,windows,count):range_model::predict(f);
    std::cout << std::setprecision(10) << p.cls << ' ' << p.accepted;
    for (float v:p.probabilities) std::cout << ' ' << v;
    std::cout << '\n';
  }
}
