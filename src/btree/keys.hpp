#ifndef BTREE_KEYS_HPP_
#define BTREE_KEYS_HPP_

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "config/args.hpp"
#include "rpc/serialize_macros.hpp"
#include "utils.hpp"

// Note: Changing this struct changes the format of the data stored on disk.
// If you change this struct, previous stored data will be misinterpreted.
struct btree_key_t {
    uint8_t size;
    uint8_t contents[];
    uint16_t full_size() const {
        return size + offsetof(btree_key_t, contents);
    }
    bool fits(int space) const {
        return space > 0 && space > size;
    }
    void print() const {
        debugf("%*.*s\n", size, size, contents);
    }
};

struct store_key_t {
public:
    store_key_t() {
        set_size(0);
    }

    store_key_t(int sz, const uint8_t *buf) {
        assign(sz, buf);
    }

    store_key_t(const store_key_t& key_) {
        assign(key_.size(), key_.contents());
    }

    explicit store_key_t(const btree_key_t *key) {
        assign(key->size, key->contents);
    }

    explicit store_key_t(const std::string& s) {
        assign(s.size(), reinterpret_cast<const uint8_t *>(s.data()));
    }

    btree_key_t *btree_key() { return reinterpret_cast<btree_key_t *>(buffer); }
    const btree_key_t *btree_key() const { return reinterpret_cast<const btree_key_t *>(buffer); }
    void set_size(int s) {
        rassert(s <= MAX_KEY_SIZE);
        btree_key()->size = s;
    }
    int size() const { return btree_key()->size; }
    uint8_t *contents() { return btree_key()->contents; }
    const uint8_t *contents() const { return btree_key()->contents; }

    void assign(int sz, const uint8_t *buf) {
        set_size(sz);
        memcpy(contents(), buf, sz);
    }

    void assign(const btree_key_t *key) {
        assign(key->size, key->contents);
    }

    void print() const {
        printf("%*.*s", size(), size(), contents());
    }

    static store_key_t min() {
        return store_key_t(0, NULL);
    }

    static store_key_t max() {
        uint8_t buf[MAX_KEY_SIZE];
        for (int i = 0; i < MAX_KEY_SIZE; i++) {
            buf[i] = 255;
        }
        return store_key_t(MAX_KEY_SIZE, buf);
    }

    bool increment() {
        if (size() < MAX_KEY_SIZE) {
            contents()[size()] = 0;
            set_size(size() + 1);
            return true;
        }
        while (size() > 0 && contents()[size()-1] == 255) {
            set_size(size() - 1);
        }
        if (size() == 0) {
            /* We were the largest possible key. Oops. Restore our previous
            state and return `false`. */
            *this = store_key_t::max();
            return false;
        }
        (reinterpret_cast<uint8_t *>(contents()))[size()-1]++;
        return true;
    }

    bool decrement() {
        if (size() == 0) {
            return false;
        } else if ((reinterpret_cast<uint8_t *>(contents()))[size()-1] > 0) {
            (reinterpret_cast<uint8_t *>(contents()))[size()-1]--;
            for (int i = size(); i < MAX_KEY_SIZE; i++) {
                contents()[i] = 255;
            }
            set_size(MAX_KEY_SIZE);
            return true;
        } else {
            set_size(size() - 1);
            return true;
        }
    }

    int compare(const store_key_t& k) const {
        return sized_strcmp(contents(), size(), k.contents(), k.size());
    }

    void rdb_serialize(write_message_t &msg) const {
        uint8_t sz = size();
        msg << sz;
        msg.append(contents(), sz);
    }

    template <class T> friend int deserialize(read_stream_t *, T *);
    int rdb_deserialize(read_stream_t *s) {
        uint8_t sz;
        int res = deserialize(s, &sz);
        if (res) { return res; }
        int64_t num_read = force_read(s, contents(), sz);
        if (num_read == -1) {
            return -1;
        }
        if (num_read < sz) {
            return -2;
        }
        rassert(num_read == sz);
        set_size(sz);
        return 0;
    }

private:
    char buffer[sizeof(btree_key_t) + MAX_KEY_SIZE];
};

inline bool operator==(const store_key_t &k1, const store_key_t &k2) {
    return k1.size() == k2.size() && memcmp(k1.contents(), k2.contents(), k1.size()) == 0;
}

inline bool operator!=(const store_key_t &k1, const store_key_t &k2) {
    return !(k1 == k2);
}

inline bool operator<(const store_key_t &k1, const store_key_t &k2) {
    return k1.compare(k2) < 0;
}

inline bool operator>(const store_key_t &k1, const store_key_t &k2) {
    return k2 < k1;
}

inline bool operator<=(const store_key_t &k1, const store_key_t &k2) {
    return k1.compare(k2) <= 0;
}

inline bool operator>=(const store_key_t &k1, const store_key_t &k2) {
    return k2 <= k1;
}

bool unescaped_str_to_key(const char *str, store_key_t *buf);
std::string key_to_unescaped_str(const store_key_t &key);

std::string key_to_debug_str(const store_key_t &key);

/* `key_range_t` represents a contiguous set of keys. */
struct key_range_t {
    enum bound_t {
        open,
        closed,
        none
    };

    key_range_t();   /* creates a range containing no keys */
    key_range_t(bound_t, const store_key_t&, bound_t, const store_key_t&);

    static key_range_t empty() THROWS_NOTHING {
        return key_range_t();
    }

    static key_range_t universe() THROWS_NOTHING {
        store_key_t k;
        return key_range_t(key_range_t::none, k, key_range_t::none, k);
    }

    bool is_empty() const {
        return !right.unbounded && left >= right.key;
    }

    bool contains_key(const store_key_t& key) const {
        bool left_ok = left <= key;
        bool right_ok = right.unbounded || key < right.key;
        return left_ok && right_ok;
    }

    bool contains_key(const uint8_t *key, uint8_t size) const {
        bool left_ok = sized_strcmp(left.contents(), left.size(), key, size) <= 0;
        bool right_ok = right.unbounded || sized_strcmp(key, size, right.key.contents(), right.key.size()) < 0;
        return left_ok && right_ok;
    }

    /* If `right.unbounded`, then the range contains all keys greater than or
    equal to `left`. If `right.bounded`, then the range contains all keys
    greater than or equal to `left` and less than `right.key`. */
    struct right_bound_t {
        right_bound_t() : unbounded(true) { }
        explicit right_bound_t(store_key_t k) : unbounded(false), key(k) { }
        bool unbounded;
        store_key_t key;

        RDB_MAKE_ME_SERIALIZABLE_2(unbounded, key);
    };
    store_key_t left;
    right_bound_t right;

    RDB_MAKE_ME_SERIALIZABLE_2(left, right);
};

std::string key_range_to_debug_str(const key_range_t &kr);

bool operator==(const key_range_t::right_bound_t &a, const key_range_t::right_bound_t &b);
bool operator!=(const key_range_t::right_bound_t &a, const key_range_t::right_bound_t &b);
bool operator<(const key_range_t::right_bound_t &a, const key_range_t::right_bound_t &b);
bool operator<=(const key_range_t::right_bound_t &a, const key_range_t::right_bound_t &b);
bool operator>(const key_range_t::right_bound_t &a, const key_range_t::right_bound_t &b);
bool operator>=(const key_range_t::right_bound_t &a, const key_range_t::right_bound_t &b);

bool operator==(key_range_t, key_range_t) THROWS_NOTHING;
bool operator!=(key_range_t, key_range_t) THROWS_NOTHING;
bool operator<(const key_range_t &, const key_range_t &) THROWS_NOTHING;

#endif // BTREE_KEYS_HPP_
