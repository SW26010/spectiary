#include <cstdio>
#include <cstdlib>

#ifndef __SANITIZE_ADDRESS__
#error This detection probe must be compiled with AddressSanitizer.
#endif

__declspec(noinline) int Read(const int* values, int index)
{
    return values[index];
}

int main(int argc, char**)
{
    auto* values = static_cast<int*>(std::malloc(4 * sizeof(int)));
    if (!values) {
        return 2;
    }
    values[0] = 42;
    // A separate child process deliberately reads beyond this allocation.
    // Keep the index dynamic so the compiler cannot fold the invalid access.
    const int value = Read(values, argc == 1 ? 0 : argc * 4);
    std::printf("value=%d\n", value);
    std::free(values);
    return value == 42 ? 0 : 3;
}
