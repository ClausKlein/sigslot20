#include <sigslot/signal.hpp>
#include <iostream>

void f() { std::cout << "free function\n"; }

class s {
public:
    void m() const { std::cout << "member function: v=" << v << std::endl; }
    static void sm() { std::cout << "static member function\n";  }

private:
    const int v{123};
};

struct o {
    void operator()() { std::cout << "function object\n"; }
};

int main() {
    s d;
    auto lambda = []() { std::cout << "lambda\n"; };

    // declare a signal instance with no arguments
    sigslot::signal<> sig;

    // sigslot::signal will connect to any callable provided it has compatible
    // arguments. Here are diverse examples
    sig.connect(f);
    sig.connect(&s::m, &d);
    sig.connect(&s::sm);
    sig.connect(o());
    sig.connect(lambda);

    // Avoid hitting bug https://gcc.gnu.org/bugzilla/show_bug.cgi?id=68071
    // on old GCC compilers
#ifndef __clang__
#if GCC_VERSION > 70300
    auto gen_lambda = [](auto && ... /*a*/) { std::cout << "generic lambda\n"; };
    sig.connect(gen_lambda);
#endif
#endif

    // emit a signal
    sig();

    return 0;
}
