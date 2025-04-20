#pragma once
#include <atomic>
#include <functional>
#include <array>
#include <cstddef>

// Forward declarations
struct Order;
struct Limit;

// ==================== Order ====================
struct Order {
    int             idNumber;
    bool            buyOrSell;
    int             shares;
    int             limitPrice;
    int             entryTime;
    int             eventTime;
    Order*          nextOrder;
    Order*          prevOrder;
    Limit*          parentLimit;
};

// ==================== Limit (tree node) ====================
struct Limit {
    int                     limitPrice;
    std::atomic<int>        size;
    std::atomic<long long>  totalVolume;

    // BST links
    std::atomic<Limit*>     parent;
    std::atomic<Limit*>     leftChild;
    std::atomic<Limit*>     rightChild;

    // Per-limit doubly-linked order list
    std::atomic<Order*>     headOrder;
    std::atomic<Order*>     tailOrder;

    Limit(int price)
      : limitPrice(price), size(0), totalVolume(0),
        parent(nullptr), leftChild(nullptr), rightChild(nullptr),
        headOrder(nullptr), tailOrder(nullptr)
    {}
};

// ==================== LockFreeHashMap ====================
// Simple, fixed-size, separate-chaining hash map (no dynamic resizing)
template<typename K, typename V, std::size_t BUCKETS = 1024>
class LockFreeHashMap {
    struct Node {
        K                   key;
        std::atomic<V>      value;
        std::atomic<Node*>  next;
        Node(const K& k, V v)
          : key(k), value(v), next(nullptr)
        {}
    };

    std::array<std::atomic<Node*>, BUCKETS> buckets;
    std::hash<K> hasher;

public:
    LockFreeHashMap() {
        for (auto &b : buckets) {
            b.store(nullptr, std::memory_order_relaxed);
        }
    }

    // Insert or overwrite
    bool insert(const K& key, V val) {
        std::size_t idx = hasher(key) % BUCKETS;
        Node* newNode = new Node(key, val);
        while (true) {
            Node* head = buckets[idx].load(std::memory_order_acquire);
            newNode->next.store(head, std::memory_order_relaxed);
            if (buckets[idx].compare_exchange_weak(
                    head, newNode,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                return true;
            }
        }
    }

    // Find; returns nullptr if not present or erased
    V find(const K& key) {
        std::size_t idx = hasher(key) % BUCKETS;
        for (Node* cur = buckets[idx].load(std::memory_order_acquire);
             cur;
             cur = cur->next.load(std::memory_order_acquire))
        {
            if (cur->key == key) {
                V v = cur->value.load(std::memory_order_acquire);
                if (v) return v;
            }
        }
        return V();
    }

    // Logical erase: mark value as default(V)
    V erase(const K& key) {
        std::size_t idx = hasher(key) % BUCKETS;
        for (Node* cur = buckets[idx].load(std::memory_order_acquire);
             cur;
             cur = cur->next.load(std::memory_order_acquire))
        {
            if (cur->key == key) {
                V old = cur->value.load(std::memory_order_acquire);
                if (old && cur->value.compare_exchange_strong(
                        old, V(),
                        std::memory_order_acq_rel))
                {
                    return old;
                }
            }
        }
        return V();
    }
};

// ==================== OrderBook ====================
class OrderBook {
    // BST roots
    std::atomic<Limit*>    buyTree{nullptr};
    std::atomic<Limit*>    sellTree{nullptr};
    // "Inside" pointers
    std::atomic<Limit*>    highestBuy{nullptr};
    std::atomic<Limit*>    lowestSell{nullptr};

    // Lock-free maps
    LockFreeHashMap<int, Order*> orderIndex;
    LockFreeHashMap<int, Limit*> limitIndex;

public:
    // Lookup-or-insert a price level in the correct BST
    Limit* findOrInsertLimit(bool isBuySide, int price) {
        // 1) try hash-index
        if (auto L = limitIndex.find(price)) {
            return L;
        }

        // 2) BST insert via CAS
        auto& root = isBuySide ? buyTree : sellTree;
        while (true) {
            Limit* parent = nullptr;
            Limit* cur    = root.load(std::memory_order_acquire);
            while (cur) {
                if (price == cur->limitPrice) {
                    limitIndex.insert(price, cur);
                    return cur;
                }
                parent = cur;
                cur = (price < cur->limitPrice)
                      ? cur->leftChild.load(std::memory_order_acquire)
                      : cur->rightChild.load(std::memory_order_acquire);
            }

            Limit* newNode = new Limit(price);
            if (!parent) {
                // Empty tree
                if (root.compare_exchange_strong(
                        cur, newNode,
                        std::memory_order_acq_rel))
                {
                    updateInsidePointer(isBuySide, newNode);
                    limitIndex.insert(price, newNode);
                    return newNode;
                }
            } else {
                // Attach under parent
                auto& link = (price < parent->limitPrice)
                              ? parent->leftChild
                              : parent->rightChild;
                if (link.compare_exchange_strong(
                        cur, newNode,
                        std::memory_order_acq_rel))
                {
                    newNode->parent.store(parent,
                                           std::memory_order_release);
                    updateInsidePointer(isBuySide, newNode);
                    limitIndex.insert(price, newNode);
                    return newNode;
                }
            }

            delete newNode;
            // retry
        }
    }

    // Insert a new order
    void onNewOrder(int id, bool buy, int shares,
                    int price, int entryTime, int eventTime)
    {
        Order* o = new Order{id, buy, shares, price,
                             entryTime, eventTime,
                             nullptr, nullptr, nullptr};
        Limit* L = findOrInsertLimit(buy, price);

        // Append to per-limit list (lock-free push to tail)
        while (true) {
            Order* tail = L->tailOrder.load(std::memory_order_acquire);
            o->prevOrder = tail;
            o->nextOrder = nullptr;
            if (L->tailOrder.compare_exchange_weak(
                    tail, o,
                    std::memory_order_acq_rel))
            {
                if (tail) tail->nextOrder = o;
                else    L->headOrder.store(o,
                                             std::memory_order_release);
                break;
            }
        }
        o->parentLimit = L;
        L->size.fetch_add(1, std::memory_order_relaxed);
        L->totalVolume.fetch_add(shares, std::memory_order_relaxed);
        orderIndex.insert(id, o);
    }

    // Cancel an existing order
    Order* onCancel(int id) {
        if (Order* o = orderIndex.erase(id)) {
            Limit* L = o->parentLimit;
            Order* prev = o->prevOrder;
            Order* next = o->nextOrder;
            if (prev) prev->nextOrder = next;
            else        L->headOrder.store(next,
                                            std::memory_order_release);
            if (next) next->prevOrder = prev;
            else        L->tailOrder.store(prev,
                                            std::memory_order_release);
            L->size.fetch_sub(1, std::memory_order_relaxed);
            L->totalVolume.fetch_sub(o->shares,
                                    std::memory_order_relaxed);
            return o;
        }
        return nullptr;
    }

    // (optional) access best bid/ask
    Limit* bestBid() const { return highestBuy.load(); }
    Limit* bestAsk() const { return lowestSell.load(); }

private:
    // Update highestBuy / lowestSell via CAS
    void updateInsidePointer(bool isBuySide, Limit* cand) {
        auto& inside = isBuySide ? highestBuy : lowestSell;
        Limit* old = nullptr;
        while (true) {
            old = inside.load(std::memory_order_acquire);
            if (old) {
                if ((isBuySide  && old->limitPrice >= cand->limitPrice) ||
                    (!isBuySide && old->limitPrice <= cand->limitPrice))
                {
                    return;
                }
            }
            if (inside.compare_exchange_weak(
                    old, cand,
                    std::memory_order_acq_rel))
            {
                return;
            }
        }
    }
};
