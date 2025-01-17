#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <thread>
#include <vector>
#include <optional>
#include <concepts>

#if defined __clang__ || (__GNUC__ > 5)
#define SIGSLOT_MAY_ALIAS __attribute__((__may_alias__))
#else
#define SIGSLOT_MAY_ALIAS
#endif

#if defined(__GXX_RTTI) || defined(__cpp_rtti) || defined(_CPPRTTI)
#define SIGSLOT_RTTI_ENABLED 1
#include <typeinfo>
#endif

namespace sigslot {

namespace detail {

// Used to detect an object of observer type
struct observer_type {};

} // namespace detail

namespace trait {

/**
 * Pointers that can be converted to a weak pointer concept for tracking
 * purpose must implement the to_weak() function in order to make use of
 * ADL to convert that type and make it usable
 */

template <typename T>
std::weak_ptr<T> to_weak(std::weak_ptr<T> w) {
    return w;
}

template <typename T>
std::weak_ptr<T> to_weak(std::shared_ptr<T> s) {
    return s;
}

static constexpr bool with_rtti =
#ifdef SIGSLOT_RTTI_ENABLED
        true;
#else
        false;
#endif

template<typename F, typename... T>
concept Callable = requires(F f, T... ts) {
    f(ts...);
};

template<typename F, typename P, typename... T>
concept MemberCallable = requires(F f, P p, T... ts) {
    ((*p).*f)(ts...);
};

template<typename T>
concept WeakPtr = requires(T p) {
    p.expired();
    p.lock();
    p.reset();
};

template<typename T>
concept WeakPtrCompatible = requires(T t) {
    { to_weak(t) } -> WeakPtr;
};

template<typename T>
concept Functor = std::is_member_function_pointer_v<
                        decltype(&std::remove_reference_t<T>::operator())>;

template<typename T>
concept Pointer = std::is_pointer_v<T>;

template<typename T>
concept Function = std::is_function_v<T>;

template<typename T>
concept MemFnPointer = std::is_member_function_pointer_v<T>;

template<typename T>
concept Observer = std::is_base_of_v<::sigslot::detail::observer_type,
                                     std::remove_pointer_t<T>>;

} // namespace trait

template<typename T>
concept GroupId = requires(T g1, T g2) {
    requires std::is_default_constructible_v<T>;
    requires std::is_copy_constructible_v<T>;
    { g1 < g2 } -> std::same_as<bool>;
    { g1 == g2 } -> std::same_as<bool>;
};

template <GroupId, typename, typename...>
class signal_base;

namespace detail {

/**
 * The following function_traits and object_pointer series of templates are
 * used to circumvent the type-erasing that takes place in the slot_base
 * implementations. They are used to compare the stored functions and objects
 * with another one for disconnection purpose.
 */

/*
 * Function pointers and member function pointers size differ from compiler to
 * compiler, and for virtual members compared to non virtual members. On some
 * compilers, multiple inheritance has an impact too. Hence, we form an union
 * big enough to store any kind of function pointer.
 */
namespace mock {

struct a { virtual ~a() = default; void f(); virtual void g(); };
struct b { virtual ~b() = default; virtual void h(); };
struct c : a, b { void g() override; };

union fun_types {
    decltype(&c::g) m;
    decltype(&a::g) v;
    decltype(&a::f) d;
    void (*f)();
    void *o;
 };

} // namespace mock

/*
 * This union is used to compare function pointers
 * Generic callables cannot be compared. Here we compare pointers but there is
 * no guarantee that this always works.
 */
union SIGSLOT_MAY_ALIAS func_ptr {
    void* value() {
        return &data[0];
    }

    [[nodiscard]] const void* value() const {
        return &data[0];
    }

    template <typename T>
    T& value() {
        return *static_cast<T*>(value());
    }

    template <typename T>
    [[nodiscard]] [[nodiscard]] [[nodiscard]] [[nodiscard]] [[nodiscard]] const T& value() const {
        return *static_cast<const T*>(value());
    }

    inline explicit operator bool() const {
        return value() != nullptr;
    }

    inline bool operator==(const func_ptr &o) const {
        return std::equal(std::begin(data), std::end(data), std::begin(o.data));
    }

    mock::fun_types _;
    // NOLINTNEXTLINE(hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
    char data[sizeof(mock::fun_types)]; // TODO(CK) use std::array<> instead!
};

template <typename T>
struct function_traits {
    static void ptr(const T &/*t*/, func_ptr &d) {
        d.value<std::nullptr_t>() = nullptr;
    }

    static constexpr bool is_disconnectable = false;
    static constexpr bool must_check_object = true;
};

template <trait::Function T>
struct function_traits<T> {
    static void ptr(T &t, func_ptr &d) {
        d.value<T*>() = &t;
    }

    static constexpr bool is_disconnectable = true;
    static constexpr bool must_check_object = false;
};

template <trait::Function T>
struct function_traits<T*> {
    static void ptr(T *t, func_ptr &d) {
        d.value<T*>() = t;
    }

    static constexpr bool is_disconnectable = true;
    static constexpr bool must_check_object = false;
};

template <trait::MemFnPointer T>
struct function_traits<T> {
    static void ptr(const T &t, func_ptr &d) {
        d.value<T>() = t;
    }

    static constexpr bool is_disconnectable = trait::with_rtti;
    static constexpr bool must_check_object = true;
};

// for function objects, the assumption is that we are looking for the call operator
template <trait::Functor T>
struct function_traits<T> {
    using call_type = decltype(&std::remove_reference<T>::type::operator());

    static void ptr(const T &/*t*/, func_ptr &d) {
        function_traits<call_type>::ptr(&T::operator(), d);
    }

    static constexpr bool is_disconnectable = function_traits<call_type>::is_disconnectable;
    static constexpr bool must_check_object = function_traits<call_type>::must_check_object;
};

template <typename T>
func_ptr get_function_ptr(const T &t) {
    func_ptr d{};
    std::uninitialized_fill(std::begin(d.data), std::end(d.data), '\0');
    function_traits<std::decay_t<T>>::ptr(t, d);
    return d;
}

/*
 * obj_ptr is used to store a pointer to an object.
 * The object_pointer traits are needed to handle trackable objects correctly,
 * as they are likely to not be pointers.
 */
using obj_ptr = const void*;

template <typename T>
obj_ptr get_object_ptr(const T &t);

template <typename T>
struct object_pointer {
    static obj_ptr get(const T& /*unused*/) {
        return nullptr;
    }
};

template <typename T>
requires trait::Pointer<T*>
struct object_pointer<T*> {
    static obj_ptr get(const T *t) {
        return reinterpret_cast<obj_ptr>(t);
    }
};

template <trait::WeakPtr T>
struct object_pointer<T> {
    static obj_ptr get(const T &t) {
        auto p = t.lock();
        return get_object_ptr(p);
    }
};

template <trait::WeakPtrCompatible T>
requires (!trait::Pointer<T> && !trait::WeakPtr<T>)
struct object_pointer<T>
{
    static obj_ptr get(const T &t) {
        return t ? reinterpret_cast<obj_ptr>(t.get()) : nullptr;
    }
};

template <typename T>
obj_ptr get_object_ptr(const T &t) {
    return object_pointer<T>::get(t);
}

// noop mutex for thread-unsafe use
struct null_mutex {
    null_mutex() noexcept = default;
    ~null_mutex() noexcept = default;
    null_mutex(const null_mutex &) = delete;
    null_mutex& operator=(const null_mutex &) = delete;
    null_mutex(null_mutex &&) = delete;
    null_mutex& operator=(null_mutex &&) = delete;

    inline static bool try_lock() noexcept { return true; }
    inline void lock() noexcept {}
    inline void unlock() noexcept {}
};

/**
 * A spin mutex that yields, mostly for use in benchmarks and scenarii that invoke
 * slots at a very high pace.
 * One should almost always prefer a standard mutex over this.
 */
struct spin_mutex {
    spin_mutex() noexcept = default;
    ~spin_mutex() noexcept = default;
    spin_mutex(spin_mutex const&) = delete;
    spin_mutex& operator=(const spin_mutex &) = delete;
    spin_mutex(spin_mutex &&) = delete;
    spin_mutex& operator=(spin_mutex &&) = delete;

    void lock() noexcept {
        while (true) {
            while (!state.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }

            if (try_lock()) {
                break;
            }
        }
    }

    bool try_lock() noexcept {
        return state.exchange(false, std::memory_order_acquire);
    }

    void unlock() noexcept {
        state.store(true, std::memory_order_release);
    }

private:
    std::atomic<bool> state {true};
};

/**
 * A simple copy on write container that will be used to improve slot lists
 * access efficiency in a multithreaded context.
 */
template <typename T>
class copy_on_write {
    struct payload {
        payload() = default;

        template <typename... Args>
        explicit payload(Args && ...args)
            : value(std::forward<Args>(args)...)
        {}

        // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
        std::atomic<std::size_t> count{1};
        T value{}; // NOLINT(misc-non-private-member-variables-in-classes)
    };

public:
    using element_type = T;

    copy_on_write()
        : m_data(new payload)
    {}

    template <typename U>
    requires (!std::same_as<std::decay_t<U>, copy_on_write>)
    explicit copy_on_write(U && x)
        : m_data(new payload(std::forward<U>(x)))
    {}

    copy_on_write(const copy_on_write &x) noexcept
        : m_data(x.m_data)
    {
        ++m_data->count;
    }

    copy_on_write(copy_on_write && x) noexcept
        : m_data(x.m_data)
    {
        x.m_data = nullptr;
    }

    ~copy_on_write() {
        if (m_data && (--m_data->count == 0)) {
            delete m_data;
            m_data = nullptr;
        }
    }

    copy_on_write& operator=(const copy_on_write &x) noexcept {
        if (&x != this) {
            *this = copy_on_write(x);
        }
        return *this;
    }

    copy_on_write& operator=(copy_on_write && x) noexcept  {
        auto tmp = std::move(x);
        swap(*this, tmp);
        return *this;
    }

    element_type& write() {
        if (!unique()) {
            *this = copy_on_write(read());
        }
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDelete)
        return m_data->value; // TODO(CK) error: Use of memory after it is freed?
    }

    [[nodiscard]] const element_type& read() const noexcept {
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDelete)
        return m_data->value; // TODO(CK) error: Use of memory after it is freed?
    }

    friend inline void swap(copy_on_write &x, copy_on_write &y) noexcept {
        std::swap(x.m_data, y.m_data);
    }

private:
    [[nodiscard]] bool unique() const noexcept {
        return m_data->count == 1;
    }

private:
    payload *m_data;
};

/**
 * Specializations for thread-safe code path
 */
template <typename T>
const T& cow_read(const T &v) {
    return v;
}

template <typename T>
const T& cow_read(copy_on_write<T> &v) {
    return v.read();
}

template <typename T>
T& cow_write(T &v) {
    return v;
}

template <typename T>
T& cow_write(copy_on_write<T> &v) {
    return v.write();
}

/**
 * std::make_shared instantiates a lot a templates, and makes both compilation time
 * and executable size far bigger than they need to be. We offer a make_shared
 * equivalent that will avoid most instantiations with the following tradeoffs:
 * - Not exception safe,
 * - Allocates a separate control block, and will thus make the code slower.
 */
#ifdef SIGSLOT_REDUCE_COMPILE_TIME
template <typename B, typename D, typename ...Arg>
inline std::shared_ptr<B> make_shared(Arg && ... arg) {
    return std::shared_ptr<B>(static_cast<B*>(new D(std::forward<Arg>(arg)...)));
}
#else
template <typename B, typename D, typename ...Arg>
inline std::shared_ptr<B> make_shared(Arg && ... arg) {
    return std::static_pointer_cast<B>(std::make_shared<D>(std::forward<Arg>(arg)...));
}
#endif

/* slot_state holds slot type independent state, to be used to interact with
 * slots indirectly through connection and scoped_connection objects.
 */
class slot_state {
public:
    constexpr slot_state() noexcept
        : m_index(0)
        , m_connected(true)
        , m_blocked(false)
    {}

    virtual ~slot_state() = default;

    [[nodiscard]] virtual bool connected() const noexcept { return m_connected; }

    bool disconnect() noexcept {
        bool ret = m_connected.exchange(false);
        if (ret) {
            do_disconnect();
        }
        return ret;
    }

    [[nodiscard]] bool blocked() const noexcept { return m_blocked.load(); }
    void block()   noexcept { m_blocked.store(true); }
    void unblock() noexcept { m_blocked.store(false); }

protected:
    virtual void do_disconnect() {}

    [[nodiscard]] auto index() const {
        return m_index;
    }

    auto& index() {
        return m_index;
    }

private:
    template <GroupId, typename, typename...>
    friend class ::sigslot::signal_base;

    std::size_t m_index;     // index into the array of slot pointers inside the signal
    std::atomic<bool> m_connected;
    std::atomic<bool> m_blocked;
};

template<typename Group>
class grouped_slot : public slot_state {
protected:
    explicit grouped_slot(Group const& gid)
        : slot_state()
        , m_group(gid)
    {}
public:
    [[nodiscard]] Group const& group() const {
        return m_group;
    }
private:
    const Group m_group;
};

} // namespace detail

/**
 * connection_blocker is a RAII object that blocks a connection until destruction
 */
class connection_blocker {
public:
    connection_blocker() = default;
    ~connection_blocker() noexcept { release(); }

    connection_blocker(const connection_blocker &) = delete;
    connection_blocker & operator=(const connection_blocker &) = delete;

    connection_blocker(connection_blocker && o) noexcept
        : m_state{std::move(o.m_state)}
    {}

    connection_blocker & operator=(connection_blocker && o) noexcept {
        release();
        m_state.swap(o.m_state);
        return *this;
    }

private:
    friend class connection;
    explicit connection_blocker(std::weak_ptr<detail::slot_state> s) noexcept
        : m_state{std::move(s)}
    {
        if (auto d = m_state.lock()) {
            d->block();
        }
    }

    void release() noexcept {
        if (auto d = m_state.lock()) {
            d->unblock();
        }
    }

private:
    std::weak_ptr<detail::slot_state> m_state;
};


/**
 * A connection object allows interaction with an ongoing slot connection
 *
 * It allows common actions such as connection blocking and disconnection.
 * Note that connection is not a RAII object, one does not need to hold one
 * such object to keep the signal-slot connection alive.
 */
class connection {
public:
    connection() = default;
    virtual ~connection() = default;

    connection(const connection &) noexcept = default;
    connection & operator=(const connection &) noexcept = default;
    connection(connection &&) noexcept = default;
    connection & operator=(connection &&) noexcept = default;

    [[nodiscard]] bool valid() const noexcept {
        return !m_state.expired();
    }

    [[nodiscard]] bool connected() const noexcept {
        const auto d = m_state.lock();
        return d && d->connected();
    }

    bool disconnect() noexcept {
        auto d = m_state.lock();
        return d && d->disconnect();
    }

    [[nodiscard]] bool blocked() const noexcept {
        const auto d = m_state.lock();
        return d && d->blocked();
    }

    void block() noexcept {
        if (auto d = m_state.lock()) {
            d->block();
        }
    }

    void unblock() noexcept {
        if (auto d = m_state.lock()) {
            d->unblock();
        }
    }

    [[nodiscard]] connection_blocker blocker() const noexcept {
        return connection_blocker{m_state};
    }

protected:
    template <GroupId, typename, typename...> friend class signal_base;
    explicit connection(std::weak_ptr<detail::slot_state> s) noexcept
        : m_state{std::move(s)}
    {}

    // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    std::weak_ptr<detail::slot_state> m_state;
};

/**
 * scoped_connection is a RAII version of connection
 * It disconnects the slot from the signal upon destruction.
 */
class scoped_connection final : public connection {
public:
    scoped_connection() = default;
    ~scoped_connection() override {
        disconnect();
    }

    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
    /*implicit*/ scoped_connection(const connection &c) noexcept : connection(c) {}
    // NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
    /*implicit*/ scoped_connection(connection &&c) noexcept : connection(std::move(c)) {}

    scoped_connection(const scoped_connection &) noexcept = delete;
    scoped_connection & operator=(const scoped_connection &) noexcept = delete;

    scoped_connection(scoped_connection && o) noexcept
        : connection{std::move(o.m_state)}
    {}

    scoped_connection & operator=(scoped_connection && o) noexcept {
        disconnect();
        m_state.swap(o.m_state);
        return *this;
    }

private:
    template <GroupId, typename, typename...> friend class signal_base;
    explicit scoped_connection(std::weak_ptr<detail::slot_state> s) noexcept
        : connection{std::move(s)}
    {}
};

/**
 * Observer is a base class for intrusive lifetime tracking of objects.
 *
 * This is an alternative to trackable pointers, such as std::shared_ptr,
 * and manual connection management by keeping connection objects in scope.
 * Deriving from this class allows automatic disconnection of all the slots
 * connected to any signal when an instance is destroyed.
 */
template <typename Lockable>
struct observer_base : private detail::observer_type {
    virtual ~observer_base() = default;

protected:
    /**
     * Disconnect all signals connected to this object.
     *
     * To avoid invocation of slots on a semi-destructed instance, which may happen
     * in multi-threaded contexts, derived classes should call this method in their
     * destructor. This will ensure proper disconnection prior to the destruction.
     */
    void disconnect_all() {
        std::unique_lock<Lockable> _{m_mutex};
        m_connections.clear();
    }

private:
    template <GroupId, typename, typename ...>
    friend class signal_base;

    void add_connection(connection conn) {
        std::unique_lock<Lockable> _{m_mutex};
        m_connections.emplace_back(std::move(conn));
    }

    Lockable m_mutex;
    std::vector<scoped_connection> m_connections;
};

/**
 * Specialization of observer_base to be used in single threaded contexts.
 */
using observer_st = observer_base<detail::null_mutex>;

/**
 * Specialization of observer_base to be used in multi-threaded contexts.
 */
using observer = observer_base<std::mutex>;


namespace detail {

//concept requirements for signal_interface

template <typename Sig, typename... Args>
concept ConnectCallable =
    requires(Sig sig, Args && ... args) {
        {
        sig.connect(std::forward<Args>(args)...)
        } -> std::same_as<sigslot::connection>;
    };
template <typename Sig, typename... Args>
concept ConnectExtendedCallable =
    requires(Sig sig, Args && ... args) {
        {
        sig.connect_extended(std::forward<Args>(args)...)
        } -> std::same_as<sigslot::connection>;
    };

template <typename Sig, typename... Args>
concept DisconnectCallable =
    requires(Sig sig, Args && ... args) {
        {
        sig.disconnect(std::forward<Args>(args)...)
        } -> std::same_as<size_t>;
    };

// interface for cleanable objects, used to cleanup disconnected slots
template<typename Group>
struct cleanable {
    virtual ~cleanable() = default;
    virtual void clean(grouped_slot<Group> *) = 0;
};

template <typename Group, typename...>
class slot_base;

template <typename Group, typename... T>
using slot_ptr = std::shared_ptr<slot_base<Group, T...>>;


/* A base class for slot objects. This base type only depends on slot argument
 * types, it will be used as an element in an intrusive singly-linked list of
 * slots, hence the public next member.
 */
template <typename Group, typename... Args>
class slot_base : public grouped_slot<Group> {
public:
    using group_id = Group;
    explicit slot_base(cleanable<Group> &c, group_id const& gid)
        : grouped_slot<Group>(gid)
        , cleaner(c)
    {}
    ~slot_base() override = default;

    // method effectively responsible for calling the "slot" function with
    // supplied arguments whenever emission happens.
    virtual void call_slot(Args...) = 0;

    template <typename... U>
    void operator()(U && ...u) {
        if (slot_state::connected() && !slot_state::blocked()) {
            call_slot(std::forward<U>(u)...); // NOLINT(hicpp-no-array-decay)
        }
    }

    // check if we are storing callable c
    template <typename C>
    [[nodiscard]] [[nodiscard]] [[nodiscard]] [[nodiscard]] bool has_callable(const C &c) const {
        auto cp = get_function_ptr(c);
        auto p = get_callable();
        return cp && p && cp == p;
    }

    template <typename C>
    requires (function_traits<C>::must_check_object)
    [[nodiscard]] [[nodiscard]] [[nodiscard]] [[nodiscard]] bool has_full_callable(const C &c) const {
        return has_callable(c) && check_class_type<std::decay_t<C>>();
    }

    template <typename C>
    requires (!function_traits<C>::must_check_object)
    [[nodiscard]] [[nodiscard]] [[nodiscard]] [[nodiscard]] bool has_full_callable(const C &c) const {
        return has_callable(c);
    }

    // check if we are storing object o
    template <typename O>
    [[nodiscard]] [[nodiscard]] bool has_object(const O &o) const {
        return get_object() == get_object_ptr(o);
    }

protected:
    void do_disconnect() final {
        cleaner.clean(this);
    }

    // retieve a pointer to the object embedded in the slot
    [[nodiscard]] virtual obj_ptr get_object() const noexcept {
        return nullptr;
    }

    // retieve a pointer to the callable embedded in the slot
    [[nodiscard]] virtual func_ptr get_callable() const noexcept {
        return get_function_ptr(nullptr);
    }

#ifdef SIGSLOT_RTTI_ENABLED
    // retieve a pointer to the callable embedded in the slot
    [[nodiscard]] virtual const std::type_info& get_callable_type() const noexcept {
        return typeid(nullptr);
    }

private:
    template <typename U>
    [[nodiscard]] bool check_class_type() const {
        return typeid(U) == get_callable_type();
    }

#else
    template <typename U>
    bool check_class_type() const {
        return false;
    }
#endif

private:
    cleanable<Group> &cleaner;
};

/*
 * A slot object holds state information, and a callable to to be called
 * whenever the function call operator of its slot_base base class is called.
 */
template <typename Group, typename Func, typename... Args>
class slot final : public slot_base<Group, Args...> {
public:
    template <typename F>
    constexpr slot(cleanable<Group> &c, F && f, Group const& gid)
        : slot_base<Group, Args...>(c, gid)
        , func{std::forward<F>(f)} {}

protected:
    void call_slot(Args ...args) override {
        func(args...);
    }

    [[nodiscard]] func_ptr get_callable() const noexcept override {
        return get_function_ptr(func);
    }

#ifdef SIGSLOT_RTTI_ENABLED
    [[nodiscard]] const std::type_info& get_callable_type() const noexcept override {
        return typeid(func);
    }
#endif

private:
    std::decay_t<Func> func;
};

/*
 * Variation of slot that prepends a connection object to the callable
 */
template <typename Group, typename Func, typename... Args>
class slot_extended final : public slot_base<Group, Args...> {
public:
    template <typename F>
    constexpr slot_extended(cleanable<Group> &c, F && f, Group const& gid)
        : slot_base<Group, Args...>(c, gid)
        , func{std::forward<F>(f)} {}

    connection conn; // TODO(CK): prevent public members!

protected:
    void call_slot(Args ...args) override {
        func(conn, args...);
    }

    [[nodiscard]] func_ptr get_callable() const noexcept override {
        return get_function_ptr(func);
    }

#ifdef SIGSLOT_RTTI_ENABLED
    [[nodiscard]] const std::type_info& get_callable_type() const noexcept override {
        return typeid(func);
    }
#endif

private:
    std::decay_t<Func> func;
};

/*
 * A slot object holds state information, an object and a pointer over member
 * function to be called whenever the function call operator of its slot_base
 * base class is called.
 */
template <typename Group, typename Pmf, typename Ptr, typename... Args>
class slot_pmf final : public slot_base<Group, Args...> {
public:
    template <typename F, typename P>
    constexpr slot_pmf(cleanable<Group> &c, F && f, P && p, Group const& gid)
        : slot_base<Group, Args...>(c, gid)
        , pmf{std::forward<F>(f)}
        , ptr{std::forward<P>(p)} {}

protected:
    void call_slot(Args ...args) override {
        ((*ptr).*pmf)(args...);
    }

    [[nodiscard]] func_ptr get_callable() const noexcept override {
        return get_function_ptr(pmf);
    }

    [[nodiscard]] obj_ptr get_object() const noexcept override {
        return get_object_ptr(ptr);
    }

#ifdef SIGSLOT_RTTI_ENABLED
    [[nodiscard]] const std::type_info& get_callable_type() const noexcept override {
        return typeid(pmf);
    }
#endif

private:
    std::decay_t<Pmf> pmf;
    std::decay_t<Ptr> ptr;
};

/*
 * Variation of slot that prepends a connection object to the callable
 */
template <typename Group, typename Pmf, typename Ptr, typename... Args>
class slot_pmf_extended final : public slot_base<Group, Args...> {
public:
    template <typename F, typename P>
    constexpr slot_pmf_extended(cleanable<Group> &c, F && f, P && p, Group const& gid)
        : slot_base<Group, Args...>(c, gid)
        , pmf{std::forward<F>(f)}
        , ptr{std::forward<P>(p)} {}

    connection conn; // TODO(CK): prevent public members!

protected:
    void call_slot(Args ...args) override {
        ((*ptr).*pmf)(conn, args...);
    }

    [[nodiscard]] func_ptr get_callable() const noexcept override {
        return get_function_ptr(pmf);
    }
    [[nodiscard]] obj_ptr get_object() const noexcept override {
        return get_object_ptr(ptr);
    }

#ifdef SIGSLOT_RTTI_ENABLED
    [[nodiscard]] const std::type_info& get_callable_type() const noexcept override {
        return typeid(pmf);
    }
#endif

private:
    std::decay_t<Pmf> pmf;
    std::decay_t<Ptr> ptr;
};

/*
 * An implementation of a slot that tracks the life of a supplied object
 * through a weak pointer in order to automatically disconnect the slot
 * on said object destruction.
 */
template <typename Group, typename Func, typename WeakPtr, typename... Args>
class slot_tracked final : public slot_base<Group, Args...> {
public:
    template <typename F, typename P>
    constexpr slot_tracked(cleanable<Group> &c, F && f, P && p, Group const& gid)
        : slot_base<Group, Args...>(c, gid)
        , func{std::forward<F>(f)}
        , ptr{std::forward<P>(p)}
    {}

    [[nodiscard]] bool connected() const noexcept override {
        return !ptr.expired() && slot_state::connected();
    }

protected:
    void call_slot(Args ...args) override {
        auto sp = ptr.lock();
        if (!sp) {
            slot_state::disconnect();
            return;
        }
        if (slot_state::connected()) {
            func(args...);
        }
    }

    [[nodiscard]] func_ptr get_callable() const noexcept override {
        return get_function_ptr(func);
    }

    [[nodiscard]] obj_ptr get_object() const noexcept override {
        return get_object_ptr(ptr);
    }

#ifdef SIGSLOT_RTTI_ENABLED
    [[nodiscard]] const std::type_info& get_callable_type() const noexcept override {
        return typeid(func);
    }
#endif

private:
    std::decay_t<Func> func;
    std::decay_t<WeakPtr> ptr;
};

/*
 * An implementation of a slot as a pointer over member function, that tracks
 * the life of a supplied object through a weak pointer in order to automatically
 * disconnect the slot on said object destruction.
 */
template <typename Group, typename Pmf, typename WeakPtr, typename... Args>
class slot_pmf_tracked final : public slot_base<Group, Args...> {
public:
    template <typename F, typename P>
    constexpr slot_pmf_tracked(cleanable<Group> &c, F && f, P && p, Group const& gid)
        : slot_base<Group, Args...>(c, gid)
        , pmf{std::forward<F>(f)}
        , ptr{std::forward<P>(p)}
    {}

    [[nodiscard]] bool connected() const noexcept override {
        return !ptr.expired() && slot_state::connected();
    }

protected:
    void call_slot(Args ...args) override {
        auto sp = ptr.lock();
        if (!sp) {
            slot_state::disconnect();
            return;
        }
        if (slot_state::connected()) {
            ((*sp).*pmf)(args...);
        }
    }

    [[nodiscard]] func_ptr get_callable() const noexcept override {
        return get_function_ptr(pmf);
    }

    [[nodiscard]] obj_ptr get_object() const noexcept override {
        return get_object_ptr(ptr);
    }

#ifdef SIGSLOT_RTTI_ENABLED
    [[nodiscard]] const std::type_info& get_callable_type() const noexcept override {
        return typeid(pmf);
    }
#endif

private:
    std::decay_t<Pmf> pmf;
    std::decay_t<WeakPtr> ptr;
};

} // namespace detail


/**
 * signal_base is an implementation of the observer pattern, through the use
 * of an emitting object and slots that are connected to the signal and called
 * with supplied arguments when a signal is emitted.
 *
 * signal_base is the general implementation, whose locking policy must be
 * set in order to decide thread safety guarantees. signal and signal_st
 * are partial specializations for multi-threaded and single-threaded use.
 *
 * It does not allow slots to return a value.
 *
 * Slot execution order can be constrained by assigning group ids to the slots.
 * The execution order of slots in a same group is unspecified and should not be
 * relied upon, however groups are executed in ascending group ids order. When
 * the group id of a slot is not set, it is assigned to the group 0. Group ids
 * can have any value in the range of signed 32 bit integers.
 *
 * @tparam Lockable a lock type to decide the lock policy
 * @tparam T... the argument types of the emitting and slots functions.
 */
template <GroupId Group, typename Lockable, typename... T>
class signal_base final : public detail::cleanable<Group> {
public:
    using group_id = Group;
    static constexpr bool is_thread_safe = !std::same_as<Lockable, detail::null_mutex>;

private:
    template <typename U>
    using cow_type = std::conditional_t<is_thread_safe,
                                        detail::copy_on_write<U>, U>;

    template <typename U>
    using cow_copy_type = std::conditional_t<is_thread_safe,
                                             detail::copy_on_write<U>, const U&>;

    using lock_type = std::unique_lock<Lockable>;
    using slot_base = detail::slot_base<group_id, T...>;
    using slot_ptr = detail::slot_ptr<Group, T...>;
    using slots_type = std::vector<slot_ptr>;
    struct group_type { slots_type slts; group_id gid; };
    using list_type = std::vector<group_type>;  // kept ordered by ascending gid

public:

    signal_base() noexcept : m_block(false) {}
    ~signal_base() override {
        disconnect_all();
    }

    signal_base(const signal_base&) = delete;
    signal_base & operator=(const signal_base&) = delete;

    // NOLINTNEXTLINE(hicpp-noexcept-move,performance-noexcept-move-constructor)
    signal_base(signal_base && o) /* not noexcept */
        : m_block{o.m_block.load()}
    {
        lock_type lock(o.m_mutex);
        std::swap(m_slots, o.m_slots);
    }

    // NOLINTNEXTLINE(hicpp-noexcept-move,performance-noexcept-move-constructor)
    signal_base & operator=(signal_base && o) /* not noexcept */ {
        lock_type lock1(m_mutex, std::defer_lock);
        lock_type lock2(o.m_mutex, std::defer_lock);
        std::lock(lock1, lock2);

        std::swap(m_slots, o.m_slots);
        m_block.store(o.m_block.exchange(m_block.load()));
        return *this;
    }

    /**
     * Emit a signal
     *
     * Effect: All non blocked and connected slot functions will be called
     *         with supplied arguments.
     * Safety: With proper locking (see pal::signal), emission can happen from
     *         multiple threads simultaneously. The guarantees only apply to the
     *         signal object, it does not cover thread safety of potentially
     *         shared state used in slot functions.
     *
     * @param a... arguments to emit
     */
    template <typename... U>
    void operator()(U && ...a) {
        if (m_block) {
            return;
        }

        // Reference to the slots to execute them out of the lock
        // a copy may occur if another thread writes to it.
        cow_copy_type<list_type> ref = slots_reference();

        for (const auto &group : detail::cow_read(ref)) {
            for (const auto &s : group.slts) {
                s->operator()(a...);
            }
        }
    }

    /**
     * Connect a callable of compatible arguments
     *
     * Effect: Creates and stores a new slot responsible for executing the
     *         supplied callable for every subsequent signal emission.
     * Safety: Thread-safety depends on locking policy.
     *
     * @param c a callable
     * @param gid an identifier that can be used to order slot execution
     * @return a connection object that can be used to interact with the slot
     */
    template <typename Callable>
    requires trait::Callable<Callable, T...>
    connection connect(Callable && c, group_id gid = group_id{}) {
        using slot_t = detail::slot<group_id, Callable, T...>;
        auto s = make_slot<slot_t>(std::forward<Callable>(c), gid);
        connection conn(s);
        add_slot(std::move(s));
        return conn;
    }

    /**
     * Connect a callable with an additional connection argument
     *
     * The callable's first argument must be of type connection. This overload
     * the callable to manage it's own connection through this argument.
     *
     * @param c a callable
     * @param gid an identifier that can be used to order slot execution
     * @return a connection object that can be used to interact with the slot
     */
    template <typename Callable>
    requires trait::Callable<Callable, connection&, T...>
    connection connect_extended(Callable && c, group_id gid = group_id{}) {
        using slot_t = detail::slot_extended<group_id, Callable, T...>;
        auto s = make_slot<slot_t>(std::forward<Callable>(c), gid);
        connection conn(s);
        std::static_pointer_cast<slot_t>(s)->conn = conn;
        add_slot(std::move(s));
        return conn;
    }

    /**
     * Overload of connect for pointers over member functions derived from
     * observer
     *
     * @param pmf a pointer over member function
     * @param ptr an object pointer derived from observer
     * @param gid an identifier that can be used to order slot execution
     * @return a connection object that can be used to interact with the slot
     */
    template <typename Pmf, trait::Observer Ptr>
    requires trait::MemberCallable<Pmf, Ptr, T...>
    connection connect(Pmf && pmf, Ptr && ptr, group_id gid = group_id{}) {
        using slot_t = detail::slot_pmf<group_id, Pmf, Ptr, T...>;
        auto s = make_slot<slot_t>(std::forward<Pmf>(pmf), std::forward<Ptr>(ptr), gid);
        connection conn(s);
        add_slot(std::move(s));
        ptr->add_connection(conn);
        return conn;
    }

    /**
     * Overload of connect for pointers over member functions
     *
     * @param pmf a pointer over member function
     * @param ptr an object pointer
     * @param gid an identifier that can be used to order slot execution
     * @return a connection object that can be used to interact with the slot
     */
    template <typename Pmf, typename Ptr>
    requires trait::MemberCallable<Pmf, Ptr, T...> &&
            (!trait::Observer<Ptr> && !trait::WeakPtrCompatible<Ptr>)
    connection connect(Pmf && pmf, Ptr && ptr, group_id gid = group_id{}) {
        using slot_t = detail::slot_pmf<group_id, Pmf, Ptr, T...>;
        auto s = make_slot<slot_t>(std::forward<Pmf>(pmf), std::forward<Ptr>(ptr), gid);
        connection conn(s);
        add_slot(std::move(s));
        return conn;
    }

    /**
     * Overload  of connect for pointer over member functions and
     *
     * @param pmf a pointer over member function
     * @param ptr an object pointer
     * @param gid an identifier that can be used to order slot execution
     * @return a connection object that can be used to interact with the slot
     */
    template <typename Pmf, typename Ptr>
    requires trait::MemberCallable<Pmf, Ptr, connection&, T...> &&
            (!trait::WeakPtrCompatible<Ptr>)
    connection connect_extended(Pmf && pmf, Ptr && ptr, group_id gid = group_id{}) {
        using slot_t = detail::slot_pmf_extended<group_id, Pmf, Ptr, T...>;
        auto s = make_slot<slot_t>(std::forward<Pmf>(pmf), std::forward<Ptr>(ptr), gid);
        connection conn(s);
        std::static_pointer_cast<slot_t>(s)->conn = conn;
        add_slot(std::move(s));
        return conn;
    }

    /**
     * Overload of connect for lifetime object tracking and automatic disconnection
     *
     * Ptr must be convertible to an object following a loose form of weak pointer
     * concept, by implementing the ADL-detected conversion function to_weak().
     *
     * This overload covers the case of a pointer over member function and a
     * trackable pointer of that class.
     *
     * Note: only weak references are stored, a slot does not extend the lifetime
     * of a suppied object.
     *
     * @param pmf a pointer over member function
     * @param ptr a trackable object pointer
     * @param gid an identifier that can be used to order slot execution
     * @return a connection object that can be used to interact with the slot
     */
    template <typename Pmf, trait::WeakPtrCompatible Ptr>
    requires (!trait::Callable<Pmf, T...>)
    connection connect(Pmf && pmf, Ptr && ptr, group_id gid = group_id{}) {
        using trait::to_weak;
        auto w = to_weak(std::forward<Ptr>(ptr));
        using slot_t = detail::slot_pmf_tracked<group_id, Pmf, decltype(w), T...>;
        auto s = make_slot<slot_t>(std::forward<Pmf>(pmf), w, gid);
        connection conn(s);
        add_slot(std::move(s));
        return conn;
    }

    /**
     * Overload of connect for lifetime object tracking and automatic disconnection
     *
     * Trackable must be convertible to an object following a loose form of weak
     * pointer concept, by implementing the ADL-detected conversion function to_weak().
     *
     * This overload covers the case of a standalone callable and unrelated trackable
     * object.
     *
     * Note: only weak references are stored, a slot does not extend the lifetime
     * of a suppied object.
     *
     * @param c a callable
     * @param ptr a trackable object pointer
     * @param gid an identifier that can be used to order slot execution
     * @return a connection object that can be used to interact with the slot
     */
    template <typename Callable, trait::WeakPtrCompatible Trackable>
    requires trait::Callable<Callable, T...>
    connection connect(Callable && c, Trackable && ptr, group_id gid = group_id{}) {
        using trait::to_weak;
        auto w = to_weak(std::forward<Trackable>(ptr));
        using slot_t = detail::slot_tracked<group_id, Callable, decltype(w), T...>;
        auto s = make_slot<slot_t>(std::forward<Callable>(c), w, gid);
        connection conn(s);
        add_slot(std::move(s));
        return conn;
    }

    /**
     * Creates a connection whose duration is tied to the return object
     * Use the same semantics as connect
     */
    template <typename... CallArgs>
    scoped_connection connect_scoped(CallArgs && ...args) {
        return connect(std::forward<CallArgs>(args)...);
    }

    /**
     * Disconnect slots bound to a callable
     *
     * Effect: Disconnects all the slots bound to the callable in argument.
     * Safety: Thread-safety depends on locking policy.
     *
     * If the callable is a free or static member function, this overload is always
     * available. However, RTTI is needed for it to work for pointer to member
     * functions, function objects or and (references to) lambdas, because the
     * C++ spec does not mandate the pointers to member functions to be unique.
     *
     * @param c a callable
     * @return the number of disconnected slots
     */
    template <typename Callable>
    requires (trait::Callable<Callable, T...> ||
              trait::Callable<Callable, connection&, T...> ||
              trait::MemFnPointer<Callable>)
    && detail::function_traits<Callable>::is_disconnectable
    size_t disconnect(const Callable &c) {
        return disconnect_if([&] (const auto &s) {
            return s->has_full_callable(c);
        });
    }

    /**
     * Disconnect slots bound to this object
     *
     * Effect: Disconnects all the slots bound to the object or tracked object
     *         in argument.
     * Safety: Thread-safety depends on locking policy.
     *
     * The object may be a pointer or trackable object.
     *
     * @param obj an object
     * @return the number of disconnected slots
     */
    template <typename Obj>
    requires (!trait::Callable<Obj, T...> &&
              !trait::Callable<Obj, connection&, T...> &&
              !trait::MemFnPointer<Obj>)
    size_t disconnect(const Obj &obj) {
        return disconnect_if([&] (const auto &s) {
            return s->has_object(obj);
        });
    }

    /**
     * Disconnect slots bound both to a callable and object
     *
     * Effect: Disconnects all the slots bound to the callable and object in argument.
     * Safety: Thread-safety depends on locking policy.
     *
     * For naked pointers, the Callable is expected to be a pointer over member
     * function. If obj is trackable, any kind of Callable can be used.
     *
     * @param c a callable
     * @param obj an object
     * @return the number of disconnected slots
     */
    template <typename Callable, typename Obj>
    size_t disconnect(const Callable &c, const Obj &obj) {
        return disconnect_if([&] (const auto &s) {
            return s->has_object(obj) && s->has_callable(c);
        });
    }

    /**
     * Disconnect slots in a particular group
     *
     * Effect: Disconnects all the slots in the group id in argument.
     * Safety: Thread-safety depends on locking policy.
     *
     * @param gid a group id
     * @return the number of disconnected slots
     */
    size_t disconnect(group_id gid) {
        lock_type lock(m_mutex);
        for (auto &group : detail::cow_write(m_slots)) {
            if (group.gid == gid) {
                size_t count = group.slts.size();
                group.slts.clear();
                return count;
            }
        }
        return 0;
    }

    /**
     * Disconnects all the slots
     * Safety: Thread safety depends on locking policy
     */
    void disconnect_all() {
        lock_type lock(m_mutex);
        clear();
    }

    /**
     * Blocks signal emission
     * Safety: thread safe
     */
    void block() noexcept {
        m_block.store(true);
    }

    /**
     * Blocks all slots in a given group
     * Safety: thread safe
     */
    void block(group_id const& gid) {
        lock_type lock(m_mutex);
        for (auto &group : detail::cow_write(m_slots)) {
            if(group.gid == gid) {
                for (auto& slt : group.slts) {
                    slt->block();
                }
            }
        }
    }

    /**
     * Unblocks signal emission
     * Safety: thread safe
     */
    void unblock() noexcept {
        m_block.store(false);
    }

    /**
     * Unblocks all slots in a given group
     * Safety: thread safe
     */
    void unblock(group_id const& gid) {
        lock_type lock(m_mutex);
        for (auto &group : detail::cow_write(m_slots)) {
            if(group.gid == gid) {
                for (auto& slt : group.slts) {
                    slt->unblock();
                }
            }
        }
    }

    /**
     * Tests blocking state of signal emission
     */
    [[nodiscard]] bool blocked() const noexcept {
        return m_block.load();
    }

    /**
     * Get number of connected slots
     * Safety: thread safe
     */
    size_t slot_count() noexcept {
        cow_copy_type<list_type> ref = slots_reference();
        size_t count = 0;
        for (const auto &g : detail::cow_read(ref)) {
            count += g.slts.size();
        }
        return count;
    }

protected:
    /**
     * remove disconnected slots
     */
    void clean(detail::grouped_slot<Group> *state) override {
        lock_type lock(m_mutex);
        const auto idx = state->index();
        const auto& gid = state->group();

        // find the group
        for (auto &group : detail::cow_write(m_slots)) {
            if (group.gid == gid) {
                auto &slts = group.slts;

                // ensure we have the right slot, in case of concurrent cleaning
                if (idx < slts.size() && slts[idx] && slts[idx].get() == state) {
                    std::swap(slts[idx], slts.back());
                    slts[idx]->index() = idx;
                    slts.pop_back();
                }

                return;
            }
        }
    }

private:
    // used to get a reference to the slots for reading
    inline cow_copy_type<list_type> slots_reference() {
        lock_type lock(m_mutex);
        return m_slots;
    }

    // create a new slot
    template <typename Slot, typename... A>
    inline auto make_slot(A && ...a) {
        return detail::make_shared<slot_base, Slot>(*this, std::forward<A>(a)...);
    }

    // add the slot to the list of slots of the right group
    void add_slot(slot_ptr &&s) {
        const group_id& gid = s->group();

        lock_type lock(m_mutex);
        auto &groups = detail::cow_write(m_slots);

        // find the group
        auto it = groups.begin();
        while (it != groups.end() && it->gid < gid) {
            it++;
        }

        // create a new group if necessary
        if (it == groups.end() || it->gid != gid) {
            it = groups.insert(it, {{}, gid});
        }

        // add the slot
        s->index() = it->slts.size();
        it->slts.push_back(std::move(s));
    }

    // disconnect a slot if a condition occurs
    template <typename Cond>
    size_t disconnect_if(Cond && cond) {
        lock_type lock(m_mutex);
        auto &groups = detail::cow_write(m_slots);

        size_t count = 0;

        for (auto &group : groups) {
            auto &slts = group.slts;
            size_t i = 0;
            while (i < slts.size()) {
                if (cond(slts[i])) {
                    std::swap(slts[i], slts.back());
                    slts[i]->index() = i;
                    slts.pop_back();
                    ++count;
                } else {
                    ++i;
                }
            }
        }

        return count;
    }

    // to be called under lock: remove all the slots
    void clear() {
        detail::cow_write(m_slots).clear();
    }

private:
    Lockable m_mutex;
    cow_type<list_type> m_slots;
    std::atomic<bool> m_block;
};

/**
 * signal_interface wraps a signal and allows only its owner type to invoke it.
 *
 * @tparam Sig The signal template.
 * @tparam Owner The owner type.
 * @tparam Args The signal args.
 */
template <template<typename...> typename Sig, typename Owner, GroupId Group, typename... Args>
class signal_interface final {
    using signal_type = Sig<Group, Args...>;
    std::optional<signal_type> m_sig_storage;
    signal_type* m_sig;
    friend Owner;

    template <typename... U>
    inline void operator()(U&& ... args) {
        (*m_sig)(std::forward<U>(args)...);
        }

    inline size_t slot_count() noexcept {
        return m_sig->slot_count();
        }

    inline void block() noexcept {
        m_sig->block();
        }

    inline void block(Group const& gid) {
        m_sig->block(gid);
    }

    inline void unblock(Group const& gid) {
        m_sig->unblock(gid);
    }

    inline void unblock() noexcept {
        m_sig->unblock();
        }

    [[nodiscard]] inline bool blocked() const noexcept {
        return m_sig->blocked();
        }

    // NOLINTNEXTLINE(hicpp-noexcept-move,performance-noexcept-move-constructor)
    signal_interface(signal_interface&& o) /* not noexcept */ {
        if(o.m_sig_storage.has_value()) {
            m_sig_storage = std::move(o.m_sig_storage);
            m_sig = std::addressof(*m_sig_storage);
            o.m_sig = nullptr;
        }
        else {
            std::swap(m_sig, o.m_sig);
        }
    }

    // NOLINTNEXTLINE(hicpp-noexcept-move,performance-noexcept-move-constructor)
    signal_interface& operator=(signal_interface&& o) /* not noexcept */ {
        if(m_sig != o.m_sig) {
            if(o.m_sig_storage.has_value()) {
            m_sig_storage = std::move(o.m_sig_storage);
            m_sig = std::addressof(*m_sig_storage);
            o.m_sig = nullptr;
            }
            else {
            std::swap(m_sig, o.m_sig);
            }
        }
        return *this;
    }

    ~signal_interface() = default;

public:
    using group_id = Group;
    signal_interface()
        : m_sig_storage(std::in_place)
        , m_sig(std::addressof(*m_sig_storage))
        {
        }

    explicit signal_interface(signal_type* sig)
        : m_sig_storage(std::nullopt)
        , m_sig(sig)
        {
        }

    signal_interface(signal_interface const&) = delete;
    signal_interface& operator=(signal_interface const&) = delete;

    template <typename... Ts>
    requires detail::ConnectCallable<signal_type, Ts...>
    inline connection connect(Ts&& ... args) {
        return m_sig->connect(std::forward<Ts>(args)...);
        }

    template <typename... Ts>
    requires detail::ConnectExtendedCallable<signal_type, Ts...>
    inline connection connect_extended(Ts&& ... args) {
        return m_sig->connect_extended(std::forward<Ts>(args)...);
        }

    template <typename... Ts>
    requires detail::ConnectCallable<signal_type, Ts...>
    inline scoped_connection connect_scoped(Ts&& ... args) {
        return m_sig->connect(std::forward<Ts>(args)...);
        }

    template <typename... Ts>
    requires detail::DisconnectCallable<signal_type, Ts...>
    inline size_t disconnect(Ts&& ... args) {
        return m_sig->disconnect(std::forward<Ts>(args)...);
        }

    inline void disconnect_all() {
        m_sig->disconnect_all();
        }
};

/**
 * Specialization of signal_base to be used in single threaded contexts.
 * Slot connection, disconnection and signal emission are not thread-safe.
 * The performance improvement over the thread-safe variant is not impressive,
 * so this is not very useful.
 */
template <typename... T>
using signal_st = signal_base<int32_t, detail::null_mutex, T...>;

template<GroupId Group, typename... T>
using signal_g_st = signal_base<Group, detail::null_mutex, T...>;

/**
 * Specialization of signal_base to be used in multi-threaded contexts.
 * Slot connection, disconnection and signal emission are thread-safe.
 *
 * Recursive signal emission and emission cycles are supported too.
 */
template <typename... T>
using signal = signal_base<int32_t, std::mutex, T...>;

template<GroupId Group, typename... T>
using signal_g = signal_base<int32_t, std::mutex, T...>;

/**
 * @brief Specialization of signal_interface for single threaded signals.
 *
 * @tparam Owner The owner type. The call operator will only be accessible
 * from this type.
 * @tparam T The arguments to the signal.
 */
template<typename Owner, typename... T>
using signal_ix_st = signal_interface<signal_g_st, Owner, int32_t, T...>;

template<typename Owner, GroupId Group, typename... T>
using signal_ix_g_st = signal_interface<signal_g_st, Owner, Group, T...>;

/**
 * @brief Specialization of signal_interface for multi-threaded signals.
 *
 * @tparam Owner The owner type. The call operator will only be accessible
 * from this type.
 * @tparam T The arguments to the signal.
 */
template<typename Owner, typename... T>
using signal_ix = signal_interface<signal_g, Owner, int32_t, T...>;

template<typename Owner, GroupId Group, typename... T>
using signal_ix_g = signal_interface<signal_g, Owner, Group, T...>;
} // namespace sigslot
