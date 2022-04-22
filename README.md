# lua-cuckoofilter

lua wrapper for [cuckoofilter](https://www.cs.cmu.edu/~dga/papers/cuckoo-conext2014.pdf)

According to the paper [Cuckoo Filter: Practically Better Than Bloom
](https://www.cs.cmu.edu/~dga/papers/cuckoo-conext2014.pdf) in proceedings of ACM CoNEXT 2014 by Bin Fan, Dave Andersen and Michael Kaminsky, cuckoo filter is better than bloom filter almost in all situation. 

More, cuckoofilter can dynamically add or delete item easily rather than bloomfilter. Extend Bloom Filter may can delete item, but use more memory space and have a lower performance.

You can try it on this website [cuckoo_hashing_visualization]](http://www.lkozma.net/cuckoo_hashing_visualization/).

# implementation details

The paper intend to leave two parameters to choose:

1. Bucket size: Number of fingerprints per bucket
2. Fingerprints size: Fingerprints bits size of hashtag

In the other implementation above github, I just found the original C++ version [efficient/cuckoofilter](https://github.com/efficient/cuckoofilter) and one go version [linvon/cuckoo-filter](https://github.com/linvon/cuckoo-filter) can adjust the two parameters.

And the original C++ version have some bugs in the the Semi-sorting Buckets implementation. When I need to use cuckoofilter in lua, I decide to write a c version to use in lua myself.

Because I need a balanced filter in memory use and cpu perform, so I choose to implement the Semi-sorting Buckets in my code. Then the Bucket size is fixed to 4. I just need to adjust the Fingerprints size.

When Bucket size fixed to 4, the false positive rate is almostly 95% which feet my need.

In the end, I use a vary fast hash [komihash](https://github.com/avaneev/komihash) to generate 64 bit hash value in buckets which I have tried before.

# use

```lua
local cuckoofilter = require("cuckoofilter")

-- cuckoofilter.new(total_size, [fingerprint_size])
-- create a cuckoofilter
-- total_size is the number of key to store, must above 0.
-- fingerprint_size is the bits num of fingerprint for each item. Ranges [4, 32]
local cf = cuckoofilter.new(8190, 16)

-- cf:add(string)
-- add a string to filter, success return true.
-- otherwise return false plus reason as second result.
cf:add("asdf")

-- cf:add_unique(string)
-- add a string to filter, check if string already in.
-- success return true, otherwise as the same of cf:add.
cf:add_unique("asdf")

-- cf:contain(string)
-- check whether the string is in filter, exist return true. otherwise false.
-- notice false positive rate exist, almost 95% in when bucket size = 4
cf:contain("asdf")

-- cf:delete(string)
-- delete string from filter, success return true.
-- otherwise return false plus reason as second result.
cf:delete("asdf")

-- cf:size()
-- return the total number of item stored in filter.
cf:size()

-- cf:reset()
-- clear the filter and reset all state. no return value.
cf:reset()

-- cf:info()
-- return the state table of filter. table as below:
--[[
{
    ["bit/key"] = 22.656108597285,      // bits per item avg
    ["fingerprint_size"] = 31,          // fingerprint size
    ["bits_per_tag"] = 27,              // bits occupancy per item of table
    ["hashtable_size"] = 45063,         // bytes occupancy of table
    ["load_factor"] = 0.97119140625,    // load factor
    ["num_buckets"] = 4096,             // num of table buckets
    ["size"] = 15912,                   // add item number
    ["hashtable_capacity"] = 16384,     // num of tags that table can store
}
]]
cf:info()
```

# tip

the cuckoofilter has its limitation:

1. In insertion, the fingerprint is hashed before it is xor-ed
with the index of its current bucket to help distribute the
items uniformly in the table. the capacity of buckets must be exponential times of 2.
2. In insertion may need to kickout the current fingerprint, so the performance of insert maybe uncertain than bloomfilter.
3. If you need to support delete item. When insert same item into cuckoofilter, the number of insertion times of the same item have a upper limit equals to kb + 1 when k is the number of hash number which is 2 in my implementation, and b is 4. So when use this reposition, if you need to delete item, not to insert the same item more than 8 times.
4. As the same item insert limitation above, when delete a item, you may need to loop delete operator to ensure the item delete success.

# for chinese user

布谷鸟过滤器是一种新型过滤器算法，这点从论文是 2014 年提出的就能看出来，相对的，传统的布隆过滤器算法是 1970 年提出的。

这个算法其实就是针对布隆过滤器做了一些性能跟易用性上的优化，或者说调整。布隆过滤器是哈希之后把对应位设上就好，布谷鸟过滤器是取了哈希值的一部分存储起来。然后又用多维来支持冲突时的调整。

布谷鸟的优化在于布隆过滤器使用多个哈希函数计算特征值，而布谷鸟只用一个哈希函数，查询速度快，而且布谷鸟过滤器支持删除元素，虽然有一定限制，但至少是可以删除了。不需要像布隆过滤器一样删除需要重构整个过滤，这点应该是比较大的优化方向。

当然布谷鸟过滤的缺点也是这样的调整带来的，具体可以看上面我写的 tip。

因为我想提供给 lua 用，但我在 github 上找对应的实现的时候发现原始的 C++ 实现在半排序桶的代码里居然有 bug，主要还是位处理的问题，我也懒得提 pr 了。而 github 上 [C](
https://github.com/begeekmyfriend/CuckooFilter) 实现的一些仓库都有各种各样的问题，有的没有实现半排序桶优化，有的不支持调整指纹长度，都不太令我满意。我只好自己写了一版，我的代码基本是参考 [C++](https://github.com/efficient/cuckoofilter) 的原始实现跟一个 [Go](https://github.com/linvon/cuckoo-filter) 的版本。相对于 C++ 的版本其实我更喜欢那个 Go 的版本。

我按照我的使用习惯，只实现了针对内存压缩的优化半排序桶的版本，原始单桶的版本我没有写，觉得对我来说没有用，单桶的代码要简单的多，如果有需要的朋友可以自己看 C++ 的实现写一个，非常容易，而且原始 C++ 的单桶版本是没有 bug 的。

另外哈希函数我用了我之前写过 lua 绑定的 [komihash](https://github.com/avaneev/komihash) 算法，这个哈希算法在非加密哈希里性能非常的好，详情可以参看 [lua-komihash](https://github.com/rangercyh/lua-komihash)。

本来我打算翻译一下那篇论文的，后来发现那个 Go 版本的作者已经翻译过了，而且还写了比较好的参数解释，这里也贴出来给有兴趣的人直接看：

[论文翻译](http://www.linvon.cn/posts/cuckoo)

[参数解释](http://www.linvon.cn/posts/%E5%B8%83%E8%B0%B7%E9%B8%9F%E8%BF%87%E6%BB%A4%E5%99%A8%E5%AE%9E%E6%88%98%E6%8C%87%E5%8D%97)

另外说一下我在转化 Go 版本的时候遇到的 Go 跟 C 不太一样的一些细节：

1. 首先是 Go 的代码会特别注意字节序的问题，在处理字节数组转换的时候会有指定的 binary 库来按照大小端读取字节流，在拼接字节做整数转换的时候也会非常注意拼接的顺序，而 C/C++ 的代码很少看到有做这方面的处理，经常看到在 C/C++ 代码里一个指针顺着一个 char 数组滑动，然后强转成不同类型的整数直接赋值用。几乎很少考虑字节序问题。我想最主要的原因是 Go 从一开始就注重跨平台的情况，所以为了保证在一个平台编译能够在不同平台运行，它对字节序做了严格的对待，而 C/C++ 代码通常需要本地编译之后使用，而绝大部分平台在 CPU 这边通常是小端字节序，所以没有必要做转换，指针直接处理就好。
2. 另一个有意思的地方是 C 里面的左移右移操作符跟 Go 的处理不太一样，Go 的操作符更加直观，对字节流采用算术移动，左移右移直观填 0，而 C 的操作符需要根据整数类型跟符号来处理，有符号数采用逻辑位移，符号位补符号，虽然无符号数是算术位移，但是如果位移的位数超过了整数类型的位数，那么就会用操作数对类型位数取模后再移动。比如把一个 uint32_t 类型的数左移 33 位，就会变成左移 1 位了。针对这种情况，我自己写了两个位移宏来处理得跟 Go 类似，也没想到更好的办法，可能 C 有这样的处理，但我没有深究了。
