#include <atomic>
#include <cstdint>

// Forward‑declare
struct Order;
struct Limit;

// Your existing Order struct — we’ll only touch the link fields:
struct Order {
    int         idNumber;
    bool        buyOrSell;
    int         shares;
    int         limit;
    int         entryTime;
    int         eventTime;

    // these become atomic only if you need concurrent list ops:
    Order*      nextOrder;
    Order*      prevOrder;
    Limit*      parentLimit;
};

// Limit becomes our tree‑node:
struct Limit {
    int                  limitPrice;
    std::atomic<int>     size;         // # orders at this price
    std::atomic<int64_t> totalVolume;  // sum of shares

    // tree links
    std::atomic<Limit*>  parent;
    std::atomic<Limit*>  leftChild;
    std::atomic<Limit*>  rightChild;

    // head/tail of your per‑limit doubly‑linked order list:
    std::atomic<Order*>  headOrder;
    std::atomic<Order*>  tailOrder;

    Limit(int price)
      : limitPrice(price),
        size(0), totalVolume(0),
        parent(nullptr),
        leftChild(nullptr),
        rightChild(nullptr),
        headOrder(nullptr),
        tailOrder(nullptr)
    {}
};

class OrderBook {
    // roots of two BSTs:
    std::atomic<Limit*>  buyTree{nullptr};
    std::atomic<Limit*>  sellTree{nullptr};

    // cached “inside” pointers:
    std::atomic<Limit*>  highestBuy{nullptr};
    std::atomic<Limit*>  lowestSell{nullptr};

  public:
    // lookup or insert a Limit node in the appropriate tree
    Limit* findOrInsertLimit(bool isBuySide, int price) {
        auto& root = isBuySide ? buyTree : sellTree;
        while (true) {
            // 1) Search down to a null child
            Limit* parent = nullptr;
            Limit* cur    = root.load(std::memory_order_acquire);
            while (cur) {
                if (price == cur->limitPrice)
                    return cur;           // found existing
                parent = cur;
                if (price < cur->limitPrice)
                    cur = cur->leftChild.load(std::memory_order_acquire);
                else
                    cur = cur->rightChild.load(std::memory_order_acquire);
            }

            // 2) Not found: allocate new node
            Limit* newNode = new Limit(price);

            // 3) Try to link it in atomically
            if (!parent) {
                // Tree was empty
                if (root.compare_exchange_strong(cur, newNode,
                                                 std::memory_order_acq_rel))
                {
                    updateInsidePointer(isBuySide, newNode);
                    return newNode;
                }
            }
            else {
                auto& childPtr = (price < parent->limitPrice)
                                  ? parent->leftChild
                                  : parent->rightChild;

                if (childPtr.compare_exchange_strong(cur, newNode,
                                                     std::memory_order_acq_rel))
                {
                    newNode->parent.store(parent,
                                          std::memory_order_release);
                    updateInsidePointer(isBuySide, newNode);
                    return newNode;
                }
            }

            // CAS failed → someone else inserted there first
            delete newNode;
            // retry whole search/insert
        }
    }

  private:
    // Maintain highestBuy or lowestSell
    void updateInsidePointer(bool isBuySide, Limit* candidate) {
        auto& inside = isBuySide ? highestBuy : lowestSell;
        Limit* old;
        while (true) {
            old = inside.load(std::memory_order_acquire);
            if (old) {
                // for buy side we want max price; for sell min price
                if ((isBuySide  && old->limitPrice >= candidate->limitPrice) ||
                    (!isBuySide && old->limitPrice <= candidate->limitPrice))
                {
                    return;  // no update needed
                }
            }
            // try to swing it to our candidate
            if (inside.compare_exchange_weak(old, candidate,
                                             std::memory_order_acq_rel))
            {
                return;
            }
            // else another thread raced us → retry comparison
        }
    }
};
