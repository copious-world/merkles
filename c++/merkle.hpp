/*

*/

#pragma once

#ifndef _MERKLE_DEFS_H_
#define _MERKLE_DEFS_H_

#include <cstddef>
#include <ctime>
//
#include <string>
#include <vector>
#include <cmath>
#include <array>
#include <list>
#include <random>
#include <functional>
#include "thread_pool_user.hpp"         // ThreadPoolUser

//#include <optional>
//#include <iostream>


using namespace std;

const size_t HASH_BITS = 256;
const size_t HASH_BYTES = (256 >> 3);


typedef enum {
    AM_LEFT,
    AM_RIGHT
} which_child;

typedef enum {
    USE_COPY,
    USE_SPECIAL_KEYS
} extend_method;

typedef array<byte,HASH_BYTES> buffer;

typedef tuple<buffer, size_t, size_t> extended_hash;

typedef struct sHashNode {
    buffer                  _hash;
    size_t                  _parent_offset;
    struct sHashNode *      _p_parent;
    which_child             _which_side;
} HashNode;


/**
 * 
*/
class CumstomRandoms {
public:

    CumstomRandoms() {
        init_randoms();
    }

    void init_randoms() {
        seed_seq seq{time(0)};
        _gen32.seed(seq);
    }


    buffer gen_random_hash() {
        buffer rand_hash;
        u_int8_t n = HASH_BYTES/2;
        for ( int i = 0; i < n; i += 2 ) {
            uint16_t word = _gen32();
            rand_hash[i] = (byte)((word >> 8) & 0xFFFF);
            rand_hash[i+1] = (byte)(word & 0xFFFF);
        }
        return rand_hash;
    }

    buffer gen_hash_and_store(size_t N,size_t i) {   // number of elements on this ply ... use it to calculate the depth...
        buffer rand_hash = gen_random_hash();
        _export_hashes.push_back(extended_hash(rand_hash,N,i));
        return rand_hash;  // random bytes...  and save the for communication
    }

    mt19937                 _gen32;
    list<extended_hash>     _export_hashes;
};





/**
 * MTree
*/
class MTree  : public CumstomRandoms, public ThreadPoolUser {
public:

    MTree(size_t num_sections) : CumstomRandoms() {
        _extend_select = USE_COPY;
        initialize_tree(num_sections);
    }
    MTree() : CumstomRandoms() {
        _section_count = 0;
        _node_count = 0;
        _extend_select = USE_COPY;
    }
    virtual ~MTree() {}

public:

    /**
     * hash_data
     * 
     * default hash is a sha256 hash 
     * override :: create a subclass with this method overridden to use a better hash...
    */
    bool hash_data(char *data,size_t size,buffer& hash) {
        bool status = true;
        vector<unsigned char> hash(picosha2::k_digest_size);
        hash256(data, (data + size), hash.begin(), hash.end());
        return status;
    }

    /**
     * combine_hash
    */
    void combine_hash(buffer &h1, buffer &h2, buffer &out_hash) {
        vector<byte> results;
        results.reserve(h1.size() + h2.size());
        results.insert(results.end(), h1.begin(), h1.end());
        results.insert(results.end(), h2.begin(), h2.end());
        char *data = (char *)results.data();
        hash_data(data,results.size(),out_hash);
    }


    bool tree_initialized() { return (_section_count != 0); }


    /**
     * initialize_tree
    */
    bool initialize_tree(size_t num_sections) {
        _section_count = num_sections;
        size_t count_tree_nodes = 0;
        double l2sect = floor(log2((double)num_sections)) + 2;
        double N_tmp = pow(2,l2sect);
        count_tree_nodes = (size_t)N_tmp + 1;
        _hash_tree.reserve(count_tree_nodes);
        _node_count = count_tree_nodes;
        //
        thread_tree();
    }

    /**
     * thread_tree
    */
    void thread_tree() {
        //
        int N = _section_count;         // number elements per section
        int next_N = _section_count;    // start of section
        int prev_N = 0;                 // end of section
        //
        while ( N ) {
            for ( int i = 0; i < N; i += 2 ) {
                //
                int j = prev_N + i;
                _hash_tree[j]._which_side = AM_LEFT;
                _hash_tree[j+1]._which_side = AM_RIGHT;
                //
                size_t p_offset = (next_N + i/2);
                _hash_tree[j]._parent_offset = p_offset;
                _hash_tree[j+1]._parent_offset = p_offset;
                if ( _hash_tree[j]._parent_offset < _node_count ) {
                    _hash_tree[j]._p_parent = &_hash_tree[p_offset];
                    _hash_tree[j+1]._p_parent = &_hash_tree[p_offset];
                }
                //
            }
            prev_N = next_N;
            N = N >> 1;
            N = N % 2 ? (N+1) : N;  // turn odd number plies even
            next_N += N;
        }
        //
    }

    /**
     * up_tree
    */
    pair<HashNode *,buffer> up_tree(size_t offset,buffer hash) {
        if ( offset < _section_count ) {
            //
            vector<HashNode>::iterator vit = _hash_tree.begin();
            vit += offset;
            HashNode *hn = vit.base();
            HashNode *sib = hn;
            //
            if ( hn->_which_side == AM_LEFT ) {
                sib++;
                combine_hash(hash,sib->_hash,hash);
            } else {
                sib--;
                combine_hash(sib->_hash,hash,hash);
            }
            //
            HashNode *parent = hn->_p_parent;
            while ( parent ) {
                hn = parent;
                HashNode *sib = hn;
                if ( hn->_which_side == AM_LEFT ) {
                    sib++;
                    combine_hash(hash,sib->_hash,hash);
                } else {
                    sib--;
                    combine_hash(sib->_hash,hash,hash);
                }
                parent = hn->_p_parent;
            }
            pair<HashNode *,buffer> p(hn,hash);
            return p;
        }
        //
        pair<HashNode *,buffer> p(nullptr,hash);
        return p;
    }


    /**
     * spv_top_hash
    */
    buffer spv_top_hash(extended_hash &check_leaf,list<extended_hash> &merkle_path) {
        if ( !tree_initialized() ) {  buffer empty_b; return empty_b; }
        for ( auto hash_tpl : merkle_path ) {
            buffer &hash_val = get<0>(hash_tpl);
            size_t offset = get<2>(hash_tpl);       // absolute offset
            //
            vector<HashNode>::iterator vit = _hash_tree.begin();
            vit += offset;
            HashNode *hn = vit.base();
            hn->_hash = hash_val;
        }
        //
        buffer &hash_val = get<0>(check_leaf);
        size_t offset = get<2>(check_leaf);       // absolute offset
        pair<HashNode *,buffer> rslt = up_tree(offset,hash_val);
        if ( rslt.first != nullptr ) {
            return rslt.second;
        }
        buffer empty_b; return empty_b; 
    }


    //extended_hash &check_leaf,list<extended_hash> &merkle_path
    /**
     * 
    */
    list<extended_hash> * select_merkle_path(size_t leaf_offset) {
        //
        if ( (leaf_offset < _section_count) && (leaf_offset >= 0) ) {
            list<extended_hash> *merk_path = new list<extended_hash>();
            //
            vector<HashNode>::iterator vit = _hash_tree.begin();
            vit += leaf_offset;
            HashNode *hn = vit.base();
            HashNode *parent = hn->_p_parent;
            //
            size_t sib_offet = leaf_offset;
            uint8_t level = 0;
            //
            while( parent != nullptr ) {
                HashNode *sib = hn->_which_side == AM_LEFT ? hn + 1 : hn - 1;
                sib_offet = hn->_which_side == AM_LEFT ? (sib_offet + 1) : (sib_offet - 1);
                extended_hash ehash(sib->_hash,level,sib_offet);
                merk_path->push_back(ehash);
                //
                sib_offet = hn->_parent_offset;
                level++;
                hn = parent;
            }
            //
            return merk_path;
        }
        //
        return nullptr;
    }



    /**
     * add_data
    */
    pair<HashNode *,buffer> add_data(list<char *> &chunks, size_t chunk_size,u_int32_t worker_count) {
        if ( !tree_initialized() ) {
            pair<HashNode *,buffer> p(nullptr,gen_random_hash());
            return p;
        }
        //
        buffer hash;
        _export_hashes.clear();
        //
        HashNode *parents[_section_count];
        HashNode **p_ptr = &parents[0];
        HashNode *top = nullptr;

        if ( worker_count > 1 ) {
            bool status = true;
            //
            vector<HashNode>::iterator vit = _hash_tree.begin();
            list<char *>::iterator lit = chunks.begin();
            size_t i = 0;
            bool status = true;

            HashNode **thrd_p_ptr = p_ptr;
            // build terminal hashes from the actual chunk data
            while ( (lit != chunks.end()) && (i < _section_count) ) {  // terminals .. put them here.
                //
                char *chunk1 = *lit; lit++;
                char *chunk2 = *lit; lit++;

                HashNode *hn1 = vit.base(); vit++;
                HashNode *hn2 = vit.base(); vit++;

                enqueue_status(
                    [=] () {
                        if ( hash_data(chunk1,chunk_size,hn1->_hash) && hash_data(chunk2,chunk_size,hn2->_hash) ) {
                            HashNode *parent = hn1->_p_parent;
                            *thrd_p_ptr = parent;  // this spot is not shared
                            combine_hash(hn1->_hash,hn2->_hash,parent->_hash);
                            return true;
                        }
                        return false;
                    }
                );

                p_ptr++;

            }
            //
            status = await_status_all();
            //
            size_t N = (_section_count >> 1);
            while (N) {{
                bool extend = false;
                if ( N%2 ) { extend = true; N--; } // the number of slots on this ply.
                //
                HashNode **p_ptr = &parents[0];
                if ( (*p_ptr)->_p_parent != nullptr ) {
                    //
                    HashNode **p1_ptr = &parents[0];
                    HashNode **p2_ptr = &parents[1];
                    //
                    for ( i = 0; i < N; i += 2 ) {
                        HashNode *hn1 = *p1_ptr++;
                        HashNode *hn2 = *p2_ptr++;
                        HashNode *parent = hn1->_p_parent;
                        //
                        *p_ptr++ = parent;
                        enqueue_status(
                            [=]() {
                                combine_hash(hn1->_hash,hn2->_hash,parent->_hash);
                                return false;
                            }
                        );
                    }
                    //
                    status = await_status_all();
                    //
                } else {
                    top = *p_ptr;
                    hash = top->_hash;
                    break;
                }
                //
            }}
            //
            if ( status ) {
                pair<HashNode *,buffer> p(top,hash);
                return p;
            }
            //
        } else {
            //
            vector<HashNode>::iterator vit = _hash_tree.begin();
            list<char *>::iterator lit = chunks.begin();
            size_t i = 0;
            bool status = true;
            //
            // build terminal hashes from the actual chunk data
            while ( (lit != chunks.end()) && (i < _section_count) ) {  // terminals .. put them here.
                char *chunk = *lit;
                HashNode *hn = vit.base();
                if ( hash_data(chunk,chunk_size,hn->_hash) ) {
                    i++;
                    if ( i%2 ) {   // hn is the second of a pair
                        *p_ptr++ = hn->_p_parent;
                        combine_hash((hn-1)->_hash,hn->_hash,hn->_p_parent->_hash);
                    }
                    lit++; vit++;
                    continue;
                }
                status = false;
                break;
            }
            //
            size_t N = (_section_count >> 1);
            while (N) {{
                bool extend = false;
                if ( N%2 ) { extend = true; N--; } // the number of slots on this ply.
                //
                HashNode **p_ptr = &parents[0];
                if ( (*p_ptr)->_p_parent != nullptr ) {
                    HashNode **p1_ptr = &parents[0];
                    HashNode **p2_ptr = &parents[1];
                    //
                    for ( i = 0; i < N; i += 2 ) {
                        HashNode *hn1 = *p1_ptr++;
                        HashNode *hn2 = *p2_ptr++;
                        HashNode *parent = hn1->_p_parent;
                        //
                        *p_ptr++ = parent;
                        combine_hash(hn1->_hash,hn2->_hash,parent->_hash);
                    }
                    if ( extend ) {
                        HashNode *hn1 = *p1_ptr++;
                        HashNode *hn2 = *p2_ptr++;
                        HashNode *parent = hn1->_p_parent;
                        *p_ptr++ = parent;

                        if ( _extend_select == USE_COPY ) {
                            hn2->_hash = hn1->_hash;
                        } else {
                            hn2->_hash = gen_hash_and_store(N,i+1);   // generate a random hash to fill the spot
                        }

                        combine_hash(hn1->_hash,hn2->_hash,parent->_hash);
                    }
                } else {
                    top = *p_ptr;
                    hash = top->_hash;
                    break;
                }
                //
            }}

            if ( status ) {
                pair<HashNode *,buffer> p(top,hash);
                return p;
            }
        }

        // failed -- return basically nothing
        pair<HashNode *,buffer> p(nullptr,hash);
        return p;
    }


    /**
     * add_data_use_cores
    */
    pair<HashNode *,buffer> add_data_use_cores(list<char *> &chunks, size_t chunk_size) {
        initialize_pool();
        auto N = _thread_count;
        return add_data(chunks,chunk_size,N);
    }



public:

    size_t                  _section_count;
    size_t                  _node_count;
    extend_method           _extend_select;
    //
    vector<HashNode>        _hash_tree;   // allocate with this class instance
};









#endif //_MERKLE_DEFS_H_