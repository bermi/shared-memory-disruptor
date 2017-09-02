// Start by just creating shared memory and checking multiple processes can read
// and write to it.
// Then get the atomic operations working (add and cas).
// Then implement the algorithm. Option to clear all the memory?

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <memory>
#include <chrono>
#include <thread>
#include <napi.h>
#include <memory>
#include <vector>

typedef uint64_t sequence_t;

class Disruptor : public Napi::ObjectWrap<Disruptor>
{
public:
    Disruptor(const Napi::CallbackInfo& info);
    ~Disruptor();

    static void Initialize(Napi::Env env, Napi::Object exports);

    // Unmap the shared memory. Don't access it again from this process!
    void Release(const Napi::CallbackInfo& info);

    // Return unconsumed values for a consumer
    Napi::Value ConsumeNewSync(const Napi::CallbackInfo& info); 
    void ConsumeNewAsync(const Napi::CallbackInfo& info); 

    // Update consumer sequence without consuming more
    void ConsumeCommit(const Napi::CallbackInfo&);

    // Claim a slot for writing a value
    Napi::Value ProduceClaimSync(const Napi::CallbackInfo& info);
    void ProduceClaimAsync(const Napi::CallbackInfo& info);

    // Commit a claimed slot.
    Napi::Value ProduceCommitSync(const Napi::CallbackInfo& info);
    void ProduceCommitAsync(const Napi::CallbackInfo& info);

private:
    friend class ConsumeNewAsyncWorker;
    friend class ProduceClaimAsyncWorker;
    friend class ProduceCommitAsyncWorker;

    int Release();

    void UpdatePending(sequence_t seq_consumer, sequence_t seq_cursor);

    template<typename Array, template<typename> typename Buffer>
    Array ConsumeNewSync(const Napi::Env& env);

    void ConsumeCommit();

    template<template<typename> typename Buffer, typename Number>
    Buffer<uint8_t> ProduceClaimSync(const Napi::Env& env);

    template<typename Boolean>
    Boolean ProduceCommitSync(const Napi::Env& env, sequence_t seq_next);

    static sequence_t GetSeqNext(const Napi::CallbackInfo& info);

    uint32_t num_elements;
    uint32_t element_size;
    uint32_t num_consumers;
    uint32_t consumer;
    int32_t spin_sleep;

    size_t shm_size;
    void* shm_buf;
    
    sequence_t *consumers; // for each consumer, next slot to read
    sequence_t *cursor;    // next slot to be filled
    sequence_t *next;      // next slot to claim
    uint8_t* elements;
    sequence_t *ptr_consumer;

    sequence_t pending_seq_consumer;
    sequence_t pending_seq_cursor;
};

template <typename Result, uint32_t CB_ARG>
class DisruptorAsyncWorker : public Napi::AsyncWorker
{
public:
    DisruptorAsyncWorker(Disruptor *disruptor,
                         const Napi::CallbackInfo& info) :
        Napi::AsyncWorker(info.Length() > CB_ARG ?
            info[CB_ARG].As<Napi::Function>() :
            Napi::Function::New(info.Env(), NullCallback)),
        disruptor(disruptor),
        disruptor_ref(Napi::Persistent(disruptor->Value()))
    {
    }

protected:
    void OnOK() override
    {
        Callback().MakeCallback(
            Receiver().Value(),
            std::initializer_list<napi_value>{Env().Null(), result.ToValue(Env())});
    }

    Disruptor *disruptor;
    Result result;

private:
    static void NullCallback(const Napi::CallbackInfo& info)
    {
    }

    Napi::ObjectReference disruptor_ref;
};

template<typename T>
struct AsyncBuffer
{
    static AsyncBuffer New(Napi::Env, T* data, size_t length)
    {
        AsyncBuffer r;
        r.data = data;
        r.length = length;
        r.seq_next_set = false;
        return r;
    }

    static AsyncBuffer New(Napi::Env env, size_t length)
    {
        return New(env, nullptr, length);
    }

    void Set(const char* /* "seq_next" */, sequence_t value)
    {
        seq_next = value;
        seq_next_set = true;
    }

    Napi::Value ToValue(Napi::Env env)
    {
        Napi::Buffer<T> r;

        if (data)
        {
            r = Napi::Buffer<T>::New(env, data, length);
        }
        else
        {
            r = Napi::Buffer<T>::New(env, length);
        }

        if (seq_next_set)
        {
            r.Set("seq_next", Napi::Number::New(env, seq_next));
        }

        return r;
    }

    T* data;
    size_t length;
    sequence_t seq_next;
    bool seq_next_set;
};

template<typename T>
struct AsyncArray
{
    static AsyncArray New(Napi::Env)
    {
        AsyncArray r;
        r.elements = std::make_unique<std::vector<T>>();
        return r;
    }

    void Set(uint32_t index, T el)
    {
        if (elements->size() <= index)
        {
            elements->resize(index + 1);
        }

        (*elements)[index] = el;
    }

    Napi::Value ToValue(Napi::Env env)
    {
        size_t length = elements->size();
        Napi::Array r = Napi::Array::New(env);
        for (size_t i = 0; i < length; ++i)
        {
            r[i] = (*elements)[i].ToValue(env);
        }
        return r;
    }

    std::unique_ptr<std::vector<T>> elements;
};

struct AsyncBoolean
{
    static AsyncBoolean New(Napi::Env, bool b)
    {
        AsyncBoolean r;
        r.b = b;
        return r;
    }

    Napi::Value ToValue(Napi::Env env)
    {
        return Napi::Boolean::New(env, b);
    }

    bool b;
};

struct AsyncNumber
{
    static sequence_t New(Napi::Env, sequence_t n)
    {
        return n;
    }
};

struct CloseFD
{
    void operator()(int *fd)
    {
        close(*fd);
        delete fd;
    }
};

void ThrowErrnoError(const Napi::CallbackInfo& info, const char *msg)
{
    int errnum = errno;
    char buf[1024] = {0};
    auto errmsg = strerror_r(errnum, buf, sizeof(buf));
    static_assert(std::is_same<decltype(errmsg), char*>::value,
                  "strerror_r must return char*");
    throw Napi::Error::New(info.Env(), 
        std::string(msg) + ": " + (errmsg ? errmsg : std::to_string(errnum)));
}

Disruptor::Disruptor(const Napi::CallbackInfo& info) :
    Napi::ObjectWrap<Disruptor>(info),
    shm_buf(MAP_FAILED)
{
    // TODO: How do we ensure non-initers don't read while init is happening?
    //       (Bus error if start test2.js first then test.js)

    // Arguments
    Napi::String shm_name = info[0].As<Napi::String>();
    num_elements = info[1].As<Napi::Number>();
    element_size = info[2].As<Napi::Number>();
    num_consumers = info[3].As<Napi::Number>();
    consumer = info[4].As<Napi::Number>();
    bool init = info[5].As<Napi::Boolean>();

    if (info[6].IsNumber())
    {
        spin_sleep = info[6].As<Napi::Number>();
    }
    else
    {
        spin_sleep = -1;
    }

    // Open shared memory object
    std::unique_ptr<int, CloseFD> shm_fd(new int(
        shm_open(shm_name.Utf8Value().c_str(),
                 O_CREAT | O_RDWR | (init ? O_TRUNC : 0),
                 S_IRUSR | S_IWUSR)));
    if (*shm_fd < 0)
    {
        ThrowErrnoError(info, "Failed to open shared memory object");
    }

    // Allow space for all the elements,
    // a sequence number for each consumer,
    // the cursor sequence number (last filled slot) and
    // the next sequence number (first free slot).
    shm_size = (num_consumers + 2) * sizeof(sequence_t) +
               num_elements * element_size;

    // Resize the shared memory if we're initializing it.
    // Note: ftruncate initializes to null bytes.
    if (init && (ftruncate(*shm_fd, shm_size) < 0))
    {
        ThrowErrnoError(info, "Failed to size shared memory");
    }

    // Map the shared memory
    shm_buf = mmap(NULL,
                   shm_size,
                   PROT_READ | PROT_WRITE, MAP_SHARED,
                   *shm_fd,
                   0);
    if (shm_buf == MAP_FAILED)
    {
        ThrowErrnoError(info, "Failed to map shared memory");
    }

    consumers = static_cast<sequence_t*>(shm_buf);
    cursor = &consumers[num_consumers];
    next = &cursor[1];
    elements = reinterpret_cast<uint8_t*>(&next[1]);
    ptr_consumer = &consumers[consumer];

    pending_seq_consumer = 0;
    pending_seq_cursor = 0;
}

Disruptor::~Disruptor()
{
    Release();
}

int Disruptor::Release()
{
    if (shm_buf != MAP_FAILED)
    {
        int r = munmap(shm_buf, shm_size);

        if (r < 0)
        {
            return r;
        }

        shm_buf = MAP_FAILED;
    }

    return 0;
}

void Disruptor::Release(const Napi::CallbackInfo& info)
{
    if (Release() < 0)
    {
        ThrowErrnoError(info, "Failed to unmap shared memory");
    }
}

template<typename Array, template<typename> typename Buffer>
Array Disruptor::ConsumeNewSync(const Napi::Env& env)
{
    // Return all elements [&consumers[consumer], cursor)

    // Commit previous consume
    ConsumeCommit();

    Array r = Array::New(env);
    sequence_t seq_consumer, pos_consumer;
    sequence_t seq_cursor, pos_cursor;

    do
    {
        // TODO: Do we need to access consumer sequence atomically if we know
        // only this thread is updating it?
        // In general do we need CAS for reading?
        seq_consumer = __sync_val_compare_and_swap(ptr_consumer, 0, 0);
        seq_cursor = __sync_val_compare_and_swap(cursor, 0, 0);
        pos_consumer = seq_consumer % num_elements;
        pos_cursor = seq_cursor % num_elements;

        if (pos_cursor > pos_consumer)
        {
            r.Set(0U, Buffer<uint8_t>::New(
                env,
                elements + pos_consumer * element_size,
                (pos_cursor - pos_consumer) * element_size));

            UpdatePending(seq_consumer, seq_cursor);
            break;
        }

        if (seq_cursor != seq_consumer)
        {
            r.Set(0U, Buffer<uint8_t>::New(
                env,
                elements + pos_consumer * element_size,
                (num_elements - pos_consumer) * element_size));

            if (pos_cursor > 0)
            {
                r.Set(1U, Buffer<uint8_t>::New(
                    env,
                    elements,
                    pos_cursor * element_size));
            }

            UpdatePending(seq_consumer, seq_cursor);
            break;
        }

        if (spin_sleep < 0)
        {
            break;
        }

        if (spin_sleep > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(spin_sleep));
        }
    }
    while (true);

    return r;
}

Napi::Value Disruptor::ConsumeNewSync(const Napi::CallbackInfo& info)
{
    return ConsumeNewSync<Napi::Array, Napi::Buffer>(info.Env());
}

class ConsumeNewAsyncWorker :
    public DisruptorAsyncWorker<AsyncArray<AsyncBuffer<uint8_t>>, 0>
{
public:
    ConsumeNewAsyncWorker(Disruptor *disruptor,
                          const Napi::CallbackInfo& info) :
        DisruptorAsyncWorker<AsyncArray<AsyncBuffer<uint8_t>>, 0>(
            disruptor, info)
    {
    }

protected:
    void Execute() override
    {
        // Remember: don't access any V8 stuff in worker thread
        result = disruptor->ConsumeNewSync<AsyncArray<AsyncBuffer<uint8_t>>, AsyncBuffer>(Env());
    }
};

void Disruptor::ConsumeNewAsync(const Napi::CallbackInfo& info)
{
    (new ConsumeNewAsyncWorker(this, info))->Queue();
}

void Disruptor::ConsumeCommit(const Napi::CallbackInfo&)
{
    ConsumeCommit();
}

void Disruptor::UpdatePending(sequence_t seq_consumer, sequence_t seq_cursor)
{
    pending_seq_consumer = seq_consumer;
    pending_seq_cursor = seq_cursor;
}

void Disruptor::ConsumeCommit()
{
    if (pending_seq_cursor)
    {
        __sync_val_compare_and_swap(ptr_consumer,
                                    pending_seq_consumer,
                                    pending_seq_cursor);
        pending_seq_consumer = 0;
        pending_seq_cursor = 0;
    }
}

template<template<typename> typename Buffer, typename Number>
Buffer<uint8_t> Disruptor::ProduceClaimSync(const Napi::Env& env)
{
    sequence_t seq_next, pos_next;
    sequence_t seq_consumer, pos_consumer;
    bool can_claim;

    do
    {
        seq_next = __sync_val_compare_and_swap(next, 0, 0);
        pos_next = seq_next % num_elements;

        can_claim = true;

        for (uint32_t i = 0; i < num_consumers; ++i)
        {
            seq_consumer = __sync_val_compare_and_swap(&consumers[i], 0, 0);
            pos_consumer = seq_consumer % num_elements;

            if ((pos_consumer == pos_next) && (seq_consumer != seq_next))
            {
                can_claim = false;
                break;
            }
        }

        if (can_claim &&
            (__sync_val_compare_and_swap(next, seq_next, seq_next + 1) ==
             seq_next))
        {
            Buffer<uint8_t> r = Buffer<uint8_t>::New(
                env,
                elements + pos_next * element_size,
                element_size);

            r.Set("seq_next", Number::New(env, seq_next));

            return r;
        }

        if (spin_sleep < 0)
        {
            return Buffer<uint8_t>::New(env, 0);
        }

        if (spin_sleep > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(spin_sleep));
        }
    }
    while (true);
}

Napi::Value Disruptor::ProduceClaimSync(const Napi::CallbackInfo& info)
{
    return ProduceClaimSync<Napi::Buffer, Napi::Number>(info.Env());
}

class ProduceClaimAsyncWorker :
    public DisruptorAsyncWorker<AsyncBuffer<uint8_t>, 0>
{
public:
    ProduceClaimAsyncWorker(Disruptor *disruptor,
                            const Napi::CallbackInfo& info) :
        DisruptorAsyncWorker<AsyncBuffer<uint8_t>, 0>(
            disruptor, info)
    {
    }

protected:
    void Execute() override
    {
        // Remember: don't access any V8 stuff in worker thread
        result = disruptor->ProduceClaimSync<AsyncBuffer, AsyncNumber>(Env());
    }
};

void Disruptor::ProduceClaimAsync(const Napi::CallbackInfo& info)
{
    (new ProduceClaimAsyncWorker(this, info))->Queue();
}

template<typename Boolean>
Boolean Disruptor::ProduceCommitSync(const Napi::Env& env,
                                     sequence_t seq_next)
{
    do
    {
        if (__sync_val_compare_and_swap(cursor, seq_next, seq_next + 1) ==
            seq_next)
        {
            return Boolean::New(env, true);
        }

        if (spin_sleep < 0)
        {
            return Boolean::New(env, false);
        }

        if (spin_sleep > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(spin_sleep));
        }

    }
    while (true);
}

sequence_t Disruptor::GetSeqNext(const Napi::CallbackInfo& info)
{
    return info[0].As<Napi::Object>().Get("seq_next").As<Napi::Number>().Int64Value();
}

Napi::Value Disruptor::ProduceCommitSync(const Napi::CallbackInfo& info)
{
    return ProduceCommitSync<Napi::Boolean>(info.Env(), GetSeqNext(info));
}

class ProduceCommitAsyncWorker :
    public DisruptorAsyncWorker<AsyncBoolean, 1>
{
public:
    ProduceCommitAsyncWorker(Disruptor *disruptor,
                             const Napi::CallbackInfo& info) :
        DisruptorAsyncWorker<AsyncBoolean, 1>(disruptor, info),
        seq_next(Disruptor::GetSeqNext(info))
    {
    }

protected:
    void Execute() override
    {
        // Remember: don't access any V8 stuff in worker thread
        result = disruptor->ProduceCommitSync<AsyncBoolean>(Env(), seq_next);
    }

private:
    sequence_t seq_next;
};

void Disruptor::ProduceCommitAsync(const Napi::CallbackInfo& info)
{
    (new ProduceCommitAsyncWorker(this, info))->Queue();
}

void Disruptor::Initialize(Napi::Env env, Napi::Object exports)
{
    exports.Set("Disruptor", DefineClass(env, "Disruptor",
    {
        InstanceMethod("release", &Disruptor::Release),
        InstanceMethod("consumeNew", &Disruptor::ConsumeNewAsync),
        InstanceMethod("consumeNewSync", &Disruptor::ConsumeNewSync),
        InstanceMethod("consumeCommit", &Disruptor::ConsumeCommit),
        InstanceMethod("produceClaim", &Disruptor::ProduceClaimAsync),
        InstanceMethod("produceClaimSync", &Disruptor::ProduceClaimSync),
        InstanceMethod("produceCommit", &Disruptor::ProduceCommitAsync),
        InstanceMethod("produceCommitSync", &Disruptor::ProduceCommitSync),
    }));
}

void Initialize(Napi::Env env, Napi::Object exports, Napi::Object module)
{
    Disruptor::Initialize(env, exports);
}

NODE_API_MODULE(disruptor, Initialize)
