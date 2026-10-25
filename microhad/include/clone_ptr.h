#ifndef MICROHAD_CLONE_PTR_H
#define MICROHAD_CLONE_PTR_H
#include <memory>

// Optional ownership with value semantics. Copies never share mutable state.
template<class T> class ClonePtr {
    std::auto_ptr<T> value_;
    // Keep static default construction out of the hot accessor: RSDK otherwise
    // outlines read() itself, adding a call to every access of present state.
#if defined(__GNUC__)
    __attribute__((noinline))
#endif
    static const T& emptyValue() { static const T empty = T();return empty; }
public:
    ClonePtr() {}
    ClonePtr(const ClonePtr& other) : value_(other.present()?new T(*other.value_):0) {}
    ClonePtr& operator=(const ClonePtr& other) {
        if(this!=&other){ClonePtr copy(other);swap(copy);}return *this;
    }
    void swap(ClonePtr& other) {
        T* previous=value_.release();value_.reset(other.value_.release());other.value_.reset(previous);
    }
    bool present() const { return value_.get()!=0; }
    const T& read() const {
        if(present())return *value_;
        return emptyValue();
    }
    T& ensure() { if(!present())value_.reset(new T);return *value_; }
    void reset() { value_.reset(); }
    bool setPresent(bool present) { if(present)ensure();else reset();return present; }
};
#endif
