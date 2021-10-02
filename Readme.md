GC
====

![License](https://img.shields.io/badge/license-MIT-blue.svg)

一个基于 [Cheney's algorithm](https://en.wikipedia.org/wiki/Cheney%27s_algorithm) 的 C++ GC 实现，为增加 Pinned 及 Finalizer 支持有修改

此项目的目的在于写出一个尽可能无 UB 的 GC 实现

此项目使用了 [metaclasses](https://wg21.link/p0707) 特性，请在支持此特性的编译器下编译测试，例如 [meta](https://github.com/lock3/meta) 的 cppx 分支

不使用之时，需要自行特化实现 `GCTraits<T>::VisitPointer`，可利用特化 `GCTraits<T>::Relocate` 定制移动对象时的行为
