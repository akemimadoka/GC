// 实现 [Cheney's algorithm](https://en.wikipedia.org/wiki/Cheney%27s_algorithm)，为增加 Pinned 及 Finalizer 支持有修改

#if __has_include(<experimental/compiler>)
#define USE_METACLASSES
#include <experimental/compiler>
namespace meta = std::experimental::meta;
#endif
#include <iostream>
#include <exception>

struct AlwaysExecute
{
    constexpr bool operator()() const noexcept
    {
        return true;
    }
};

struct OnFail
{
    int UncaughtExceptions;

    OnFail()
        : UncaughtExceptions(std::uncaught_exceptions())
    {
    }

    bool operator()() const noexcept
    {
        return std::uncaught_exceptions() != UncaughtExceptions;
    }
};

struct OnSuccess
{
    int UncaughtExceptions;

    OnSuccess()
        : UncaughtExceptions(std::uncaught_exceptions())
    {
    }

    bool operator()() const noexcept
    {
        return std::uncaught_exceptions() == UncaughtExceptions;
    }
};

template <typename Handler, typename ExecutingPolicy = AlwaysExecute>
struct ScopeGuard
{
    Handler handler;
    ExecutingPolicy executingPolicy;

    ScopeGuard(Handler handler, ExecutingPolicy executingPolicy = ExecutingPolicy())
        : handler(std::move(handler))
        , executingPolicy(std::move(executingPolicy))
    {
    }

    ~ScopeGuard()
    {
        if (executingPolicy())
        {
            handler();
        }
    }
};

template <typename Handler>
using ScopeFailGuard = ScopeGuard<Handler, OnFail>;

enum class RefType
{
    // 强引用，当指针存活时，其指向的对象必定为空或指向一个存活对象，且本身存活之时，被引用的对象必定保持存活
    Strong,
    // 弱引用，暂未实现
    Weak,
};

struct GCInfo;

struct GCPtrInfo
{
    void* Ptr;
    const GCInfo* Info;
};

template <typename T>
struct PinnedGCPtr
{
    T* Value;

    ~PinnedGCPtr();

    T& operator*() const noexcept
    {
        return *Value;
    }

    T* operator->() const noexcept
    {
        return Value;
    }

    explicit operator bool() const noexcept
    {
        return Value != nullptr;
    }
};

template <typename T, RefType Ref = RefType::Strong>
struct GCPtr
{
    mutable T* Value;

    GCPtr(T* value = nullptr) noexcept;
    GCPtr(GCPtr const& other) noexcept;
    ~GCPtr();

    void* operator new(std::size_t) = delete;

    PinnedGCPtr<T> Pin() const noexcept;
    PinnedGCPtr<T> operator->() const noexcept;

    T* UnscopedPin() const noexcept;
    void UnscopedUnpin() const noexcept;

    explicit operator bool() const noexcept
    {
        return Value != nullptr;
    }
};

template <typename T>
struct GCPtrTrait
{
    static constexpr bool IsGCPtr = false;
};

template <typename T, RefType RefTypeValue>
struct GCPtrTrait<GCPtr<T, RefTypeValue>>
{
    static constexpr bool IsGCPtr = true;
    using Type = T;
    static constexpr RefType Ref = RefTypeValue;
};

#ifdef USE_METACLASSES
template <typename T>
consteval void GenerateVisitPointerBody(meta::info obj, meta::info visitor)
{
    for (const auto member : meta::data_member_range(^T))
    {
        if (meta::is_nonstatic_data_member(member))
        {
            -> fragment
            {
                using MemberType = typename [:meta::type_of(%{member}):];
                if constexpr (GCPtrTrait<MemberType>::IsGCPtr)
                {
                    static_cast<decltype([:%{visitor}:])&&>([:%{visitor}:])([:%{obj}:].[:%{member}:]);
                }
            };
        }
    }
}

template <typename T, typename Visitor>
constexpr void VisitPointer(const T& obj, Visitor&& visitor)
{
    consteval
    {
        GenerateVisitPointerBody<T>(^obj, ^visitor);
    }
}
#endif

struct Heap;

struct GCInfo;

struct GCHeader
{
    const GCInfo* Info;
    // Info 为空且 Forwardee 不为空时，指向当前空间的下一个 pinned 对象
    // 这个记录在 finalize 阶段进行
    // 为自身时是 pinned 对象
    // 为其他值时是 forwardee
    // TODO: 考虑实现为 compressed pointer
    GCHeader* Forwardee;
};

// TODO: T 直接作为基类存在问题，例如不兼容不可派生的类型，但若不如此处理，则从 T* 可能无法无 UB 地获取 GCHeader
template <typename T> requires(alignof(T) <= alignof(std::max_align_t))
struct alignas(std::max_align_t) GCObject : GCHeader, T
{
    template <typename... Args>
    constexpr explicit GCObject(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args&&...>)
        : T(std::forward<Args>(args)...)
    {
    }
};

template <typename T>
GCObject<T>* DefaultRelocate(GCObject<T>* from, GCObject<T>* to)
{
    if (std::is_trivially_move_constructible_v<T>)
    {
        std::memcpy(to, from, sizeof(GCObject<T>));
        return to;
    }
    else
    {
        return new (static_cast<void*>(to)) GCObject<T>(std::move(*static_cast<T*>(from)));
    }
}

template <typename T>
struct GCTraits
{
    template <typename Visitor>
    static constexpr void VisitPointer(const GCObject<T>& obj, Visitor&& visitor)
#ifdef USE_METACLASSES
    {
        ::VisitPointer(static_cast<const T&>(obj), std::forward<Visitor>(visitor));
    }
#else
    = delete;
#endif

    static GCObject<T>* Relocate(GCObject<T>* from, GCObject<T>* to)
    {
        return DefaultRelocate(from, to);
    }
};

constexpr std::size_t AlignTo(std::size_t size, std::size_t alignment) noexcept
{
    return (size + alignment - 1) / alignment * alignment;
}

struct GCInfo
{
    std::size_t Size;
    // ptr 是 GCPtr<T>*
    void (*Evacuate)(void* ptr, Heap* heap);
    void (*VisitPointer)(GCHeader* ptr, Heap* heap);
    void (*Finalize)(GCHeader* ptr);

    template <typename T>
    static const GCInfo& Get() noexcept;
};

struct Heap
{
    Heap()
    {
        new (static_cast<void*>(From)) GCHeader{};
        new (static_cast<void*>(To)) GCHeader{};
    }

    ~Heap()
    {
        FinalizeAll();
    }

    enum class Space
    {
        From,
        To,
    };

    enum class CollectPolicy
    {
        CollectIfNeeded,
        NeverCollect,
    };

    template <Space SpaceValue,CollectPolicy CollectPolicyValue>
    GCHeader AdjustAllocPtr(std::size_t allocatingSize)
    {
    AllocateBegin:
        const auto spaceBase = SpaceValue == Space::From ? From : To;

        if (AllocPtr - spaceBase > Size / 2 - allocatingSize)
        {
            if constexpr (CollectPolicyValue == CollectPolicy::CollectIfNeeded)
            {
                Collect();
                if (AllocPtr - spaceBase > Size / 2 - allocatingSize)
                {
                    throw std::bad_alloc{};
                }
            }
            else
            {
                throw std::bad_alloc{};
            }
        }

        GCHeader* nextPinnedObject{};
        // 若 pinned 对象尚未从连续分配的对象中分离，不可能存在下一个 pinned 对象，因此空的指示一定会一直延续
        // 若已经分离，则 Collect 已经填充了正确记录
        const auto oldHeaderContent = *std::launder(reinterpret_cast<GCHeader*>(AllocPtr));
        if (!oldHeaderContent.Info && (nextPinnedObject = oldHeaderContent.Forwardee))
        {
            // 是指示下一个 pinned 对象的 header

            // 当当前位置到下一个 pinned 对象之间的大小不足以容纳当前要求的对象及 pinned 记录大小时，跳过 pinned 对象进行测试
            if (std::launder(reinterpret_cast<std::byte*>(nextPinnedObject)) - AllocPtr < allocatingSize + sizeof(GCHeader))
            {
                // TODO: 此处浪费的空间取决于当次分配大小，且无法在下一次 Collect 之前再利用
                AllocPtr = std::launder(reinterpret_cast<std::byte*>(nextPinnedObject)) + nextPinnedObject->Info->Size;
                goto AllocateBegin;
            }
        }

        assert(!nextPinnedObject || reinterpret_cast<std::byte*>(nextPinnedObject) - AllocPtr >= sizeof(GCHeader) + allocatingSize);
        return oldHeaderContent;
    }

    template <typename T, typename... Args> requires(alignof(T) <= alignof(std::max_align_t))
    GCPtr<T> Allocate(Args&&... args)
    {
        constexpr auto AllocatingSize = sizeof(GCObject<T>);
        static_assert(AllocatingSize >= sizeof(GCHeader));

        const auto oldAllocPtr = AllocPtr;
        const auto oldHeaderContent = AdjustAllocPtr<Space::From, CollectPolicy::CollectIfNeeded>(AllocatingSize);

        const auto resultPtr = AllocPtr;
        ScopeGuard guard
        {
            [&, uncaughtExceptions = std::uncaught_exceptions()]
            {
                if (uncaughtExceptions != std::uncaught_exceptions())
                {
                    // 失败时回滚
                    AllocPtr = oldAllocPtr;
                }

                // 剩余空间不足以容纳一个 GCHeader 之时，下一次 Allocate 必定执行 Collect，不需要特别处理
                if (AllocPtr - From < Size / 2 - sizeof(GCHeader))
                {
                    new (static_cast<void*>(AllocPtr)) GCHeader(oldHeaderContent);
                }
            }
        };
        const auto obj = new (static_cast<void*>(resultPtr)) GCObject<T>(std::forward<Args>(args)...);

        const auto header = static_cast<GCHeader*>(obj);
        header->Info = &GCInfo::Get<T>();
        header->Forwardee = nullptr;
        AllocPtr += AllocatingSize;

        return GCPtr(static_cast<T*>(obj));
    }

    // TODO: 大部分逻辑和 Allocate 重复
    template <typename T>
    GCObject<T>* Evacuate(GCObject<T>* obj)
    {
        const auto header = static_cast<GCHeader*>(obj);
        if (header->Forwardee == header)
        {
            // 是 pinned 对象，不处理
            return obj;
        }

        constexpr auto AllocatingSize = sizeof(GCObject<T>);
        static_assert(AllocatingSize >= sizeof(GCHeader));

        // 空间被 pinned 对象占满等情况导致无剩余空间，无法继续进行
        if (AllocPtr - To > Size / 2 - AllocatingSize)
        {
            // TODO: 由于 Collect 是 noexcept 的，此时必定直接 std::terminate
            throw std::bad_alloc{};
        }

        const auto oldAllocPtr = AllocPtr;
        const auto oldHeaderContent = AdjustAllocPtr<Space::To, CollectPolicy::NeverCollect>(AllocatingSize);

        const auto resultPtr = AllocPtr;
        ScopeGuard guard
        {
            [&, uncaughtExceptions = std::uncaught_exceptions()]
            {
                if (uncaughtExceptions != std::uncaught_exceptions())
                {
                    // 失败时回滚
                    AllocPtr = oldAllocPtr;
                }

                // 剩余空间不足以容纳一个 GCHeader 之时，下一次 Allocate 必定执行 Collect，不需要特别处理
                if (AllocPtr - To < Size / 2 - sizeof(GCHeader))
                {
                    new (static_cast<void*>(AllocPtr)) GCHeader(oldHeaderContent);
                }
            }
        };
        const auto newObj = GCTraits<T>::Relocate(obj, reinterpret_cast<GCObject<T>*>(resultPtr));

        const auto newHeader = static_cast<GCHeader*>(newObj);
        newHeader->Info = &GCInfo::Get<T>();
        newHeader->Forwardee = nullptr;
        header->Forwardee = newHeader;
        AllocPtr += AllocatingSize;

        return newObj;
    }

    void Collect() noexcept
    {
        AllocPtr = To;
        std::byte* scanPtr = AllocPtr;

        for (std::size_t i = 0; i < CurrentGCPtrSize; ++i)
        {
            const auto ptr = OnStackGCPtrs[i].Ptr;
            assert(!IsPointerInHeap(ptr));
            OnStackGCPtrs[i].Info->Evacuate(ptr, this);
        }

        while (scanPtr < AllocPtr)
        {
            const auto header = std::launder(reinterpret_cast<GCHeader*>(scanPtr));
            if (!header->Info)
            {
                scanPtr = reinterpret_cast<std::byte*>(header->Forwardee);
                continue;
            }
            header->Info->VisitPointer(header, this);
            scanPtr += header->Info->Size;
        }

        // finalize 阶段
        // TODO: 若旧空间中不存在需要 finalize 的对象及 pinned 的对象，可以跳过此阶段
        // TODO: 若新空间中存在 pinned 的对象已 unpinned，这些对象不会在此阶段被 finalize，直到下一次 Collect 时才会处理
        auto pinnedObjectRecordHeader = std::launder(reinterpret_cast<GCHeader*>(From));
        // 因为在分配的对象到空间结尾之间如果不足以容纳一个 GCHeader，则不会写入结尾记录，所以仍然需要测试大小
        for (scanPtr = From; scanPtr - From < Size / 2 - sizeof(GCHeader);)
        {
            const auto header = std::launder(reinterpret_cast<GCHeader*>(scanPtr));
            if (!header->Info)
            {
                if (header->Forwardee)
                {
                    // 是指示下一个 pinned 对象的记录，此时到下一个 pinned 对象之间已不存在其他对象，直接跳到这个 pinned 对象
                    // 因为 pinned 对象实际可能已经 unpinned，因此需要再次检查
                    scanPtr = reinterpret_cast<std::byte*>(header->Forwardee);
                    continue;
                }

                // 已到达空间结尾，结束
                break;
            }

            const auto size = header->Info->Size;
            if (!header->Forwardee && header->Info->Finalize)
            {
                // 未被保留的对象，调用 finalize
                header->Info->Finalize(header);
            }
            else if (header->Forwardee == header)
            {
                // 是 pinned 对象

                // 如果不是紧跟在上一个 pinned 对象之后或者在空间的顶端，则需要更新 pinnedObjectRecordHeader 的内容
                if (pinnedObjectRecordHeader != header)
                {
                    new (static_cast<void*>(pinnedObjectRecordHeader)) GCHeader
                    {
                        .Info = nullptr,
                        .Forwardee = header,
                    };
                }

                // pinnedObjectRecordHeader 本身总是需要更新的
                pinnedObjectRecordHeader = std::launder(reinterpret_cast<GCHeader*>(scanPtr + size));
            }
            scanPtr += size;
        }

        if (reinterpret_cast<std::byte*>(pinnedObjectRecordHeader) < From + Size / 2 - sizeof(GCHeader))
        {
            // 未达到空间底端，因此会被测试，将其设置为空
            // 当空间不足以容纳一个 GCHeader 之时，下一个 Allocate 调用必定触发 Collect，因此不会检查接下来的空间
            new (static_cast<void*>(pinnedObjectRecordHeader)) GCHeader{};
        }

        std::swap(From, To);
    }

    // 释放两个空间中所有的对象，假定此时用户已不需要保持任何对象存活
    void FinalizeAll() noexcept
    {
        assert(CurrentGCPtrSize == 0);

        for (auto spaceBase = Space; spaceBase < Space + Size; spaceBase += Size / 2)
        {
            for (auto scanPtr = spaceBase; scanPtr - spaceBase < Size / 2 - sizeof(GCHeader);)
            {
                const auto header = std::launder(reinterpret_cast<GCHeader*>(scanPtr));
                if (!header->Info)
                {
                    if (header->Forwardee)
                    {
                        scanPtr = reinterpret_cast<std::byte*>(header->Forwardee);
                        continue;
                    }

                    break;
                }

                const auto size = header->Info->Size;
                if (!header->Forwardee && header->Info->Finalize)
                {
                    header->Info->Finalize(header);
                }
                scanPtr += size;
            }
        }
    }

    template <typename T>
    void ProcessReference(const GCPtr<T>* ptr)
    {
        const auto& info = GCInfo::Get<T>();
        if (!ptr->Value)
        {
            return;
        }

        const auto gcObj = static_cast<GCObject<T>*>(ptr->Value);
        const auto header = static_cast<GCHeader*>(gcObj);
        assert(header->Info);
        if (InFrom(gcObj))
        {
            const auto newObj = header->Forwardee ? static_cast<GCObject<T>*>(header->Forwardee) : Evacuate(gcObj);
            ptr->Value = newObj;
        }
    }

    bool IsPointerInHeap(void* ptr)
    {
        // 必须映射到整数类型再比较，否则是 UB，映射后为实现定义
        const auto address = reinterpret_cast<std::uintptr_t>(ptr);
        const auto begin = reinterpret_cast<std::uintptr_t>(Space);
        const auto end = reinterpret_cast<std::uintptr_t>(Space + Size);
        return begin <= address && address < end;
    }

    bool InFrom(void* ptr)
    {
        const auto address = reinterpret_cast<std::uintptr_t>(ptr);
        const auto begin = reinterpret_cast<std::uintptr_t>(From);
        const auto end = reinterpret_cast<std::uintptr_t>(From + Size / 2);
        return begin <= address && address < end;
    }

    // 执行 pin 操作不需要进行记录，因为若 pinned 对象已从连续分配对象中分离，则已经存在指示此对象的记录
    // 若未分离，则本身不在分配过程中需要特别处理
    template <typename T>
    void Pin(T* ptr)
    {
        const auto obj = static_cast<GCObject<T>*>(ptr);
        const auto header = static_cast<GCHeader*>(obj);
        assert(!header->Forwardee);
        header->Forwardee = header;
    }

    // 若 unpin 之时立刻进行整理似乎对空间利用率更有利，但 pin/unpin 操作在 C++ 中必然是频繁发生的
    // 因此此处不进行整理，而是在 Collect 之时再对已经 unpin 的对象进行整理
    // 不需要对记录 pinned 的 GCHeader 特别处理，因为本身仍然处于分离于连续分配的对象的状态
    template <typename T>
    void Unpin(T* ptr)
    {
        const auto obj = static_cast<GCObject<T>*>(ptr);
        const auto header = static_cast<GCHeader*>(obj);
        assert(header->Forwardee == header);
        header->Forwardee = nullptr;
    }

    static Heap& Instance()
    {
        static Heap instance{};
        return instance;
    }

    static constexpr std::size_t Size = 1024;

    alignas(std::max_align_t) std::byte Space[Size];
    std::byte* From = Space;
    std::byte* To = Space + Size / 2;
    std::byte* AllocPtr = From;

    GCPtrInfo OnStackGCPtrs[1024];
    std::size_t CurrentGCPtrSize{};

    std::size_t Used() const noexcept
    {
        return AllocPtr - From;
    }
};

template <typename T>
const GCInfo& GCInfo::Get() noexcept
{
    static constexpr GCInfo info
    {
        .Size = sizeof(GCObject<T>),
        .Evacuate = [](void* ptr, Heap* heap)
        {
            const auto gcPtr = static_cast<GCPtr<T>*>(ptr);
            gcPtr->Value = heap->Evacuate(static_cast<GCObject<T>*>(gcPtr->Value));
        },
        .VisitPointer = [](GCHeader* ptr, Heap* heap)
        {
            GCTraits<T>::VisitPointer(*static_cast<GCObject<T>*>(ptr), [=](auto& gcPtr)
            {
                heap->ProcessReference(&gcPtr);
            });
        },
        .Finalize = std::is_trivially_destructible_v<T> ? nullptr : [](GCHeader* ptr)
        {
            const auto gcObj = static_cast<GCObject<T>*>(ptr);
            gcObj->~GCObject<T>();
        },
    };
    return info;
}

template <typename T>
PinnedGCPtr<T>::~PinnedGCPtr()
{
    Heap::Instance().Unpin(Value);
}

template <typename T, RefType Ref>
PinnedGCPtr<T> GCPtr<T, Ref>::Pin() const noexcept
{
    Heap::Instance().Pin(Value);
    return { Value };
}

template <typename T, RefType Ref>
T* GCPtr<T, Ref>::UnscopedPin() const noexcept
{
    Heap::Instance().Pin(Value);
    return Value;
}

template <typename T, RefType Ref>
void GCPtr<T, Ref>::UnscopedUnpin() const noexcept
{
    Heap::Instance().Unpin(Value);
}

template <typename T, RefType Ref>
PinnedGCPtr<T> GCPtr<T, Ref>::operator->() const noexcept
{
    return Pin();
}

template <typename T, RefType Ref>
GCPtr<T, Ref>::GCPtr(T* value) noexcept
    : Value(value)
{
    auto& heap = Heap::Instance();

    if (!heap.IsPointerInHeap(this))
    {
        heap.OnStackGCPtrs[heap.CurrentGCPtrSize++] = GCPtrInfo
        {
            .Ptr = this,
            .Info = &GCInfo::Get<T>(),
        };
    }
}

template <typename T, RefType Ref>
GCPtr<T, Ref>::GCPtr(GCPtr const& other) noexcept
    : Value(other.Value)
{
    auto& heap = Heap::Instance();

    if (!heap.IsPointerInHeap(this))
    {
        heap.OnStackGCPtrs[heap.CurrentGCPtrSize++] = GCPtrInfo
        {
            .Ptr = this,
            .Info = &GCInfo::Get<T>(),
        };
    }
}

template <typename T, RefType Ref>
GCPtr<T, Ref>::~GCPtr()
{
    auto& heap = Heap::Instance();

    if (!heap.IsPointerInHeap(this))
    {
        --heap.CurrentGCPtrSize;
    }
}

struct A
{
    GCPtr<A> ptr{};

    A()
    {
        std::cout << "Constructed at " << this << std::endl;
    }

    A(A&& other) : ptr(std::move(other.ptr))
    {
        std::cout << "Relocated from " << &other << " to " << this << std::endl;
    }

    ~A()
    {
        std::cout << "Destructed at " << this << std::endl;
    }
};

int main()
{
    auto& heap = Heap::Instance();
    const auto ptr = heap.Allocate<A>();
    ptr->ptr = heap.Allocate<A>();

    heap.Allocate<A>();

    {
        const auto cycle = heap.Allocate<A>();
        cycle->ptr = heap.Allocate<A>();
        cycle->ptr->ptr = cycle;
    }

    std::cout << "Used bytes before collect: " << heap.Used() << std::endl;
    heap.Collect();
    std::cout << "Used bytes after collect: "  << heap.Used() << std::endl;

    const auto pin = heap.Allocate<A>();
    const auto pinnedRawPtr = pin.UnscopedPin();

    heap.Collect();

    // 此时 pin 应当在 To 空间中，且未被移动
    assert(pinnedRawPtr == pin.Value);
    assert(!heap.InFrom(pinnedRawPtr));
    assert(static_cast<GCHeader*>(static_cast<GCObject<A>*>(pinnedRawPtr))->Forwardee == static_cast<GCHeader*>(static_cast<GCObject<A>*>(pinnedRawPtr)));

    const auto foo = heap.Allocate<A>();

    // 此时期望的状态：
    // | From                 | To                                     |
    // | ptr, (ptr->ptr), foo | PinRecord(2 个 GCObject<A> 大小) -> pin |

    heap.Collect();

    // 此时期望的状态：
    // | From                      | To |
    // | ptr, foo, pin, (ptr->ptr) |    |

    pin.UnscopedUnpin();

    heap.Collect();

    // 旧 pinnedRawPtr 失效
    assert(pinnedRawPtr != pin.Value);

    // 此时期望的状态：
    // | From                      | To |
    // | ptr, pin, foo, (ptr->ptr) |    |

    std::cout << "End of test" << std::endl;
}
