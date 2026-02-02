//
// Created by Dell on 2.02.2026.
//
#include <iostream>

#include "lmdb/lmdb_raii.hpp"

int main()
{
    lmdb::Env env;
    env.set_maxdbs(8);
    env.set_mapsize(512ull * 1024 * 1024);
    env.open("./data", MDB_NOSUBDIR);

    // Write
    {
        lmdb::Txn wtxn(env, lmdb::Txn::Mode::ReadWrite);
        auto db = lmdb::Dbi::open(wtxn, "main", MDB_CREATE);

        MDB_val k{3, (void*)"key"};
        MDB_val v{5, (void*)"value"};
        lmdb::throw_on(mdb_put(wtxn.raw(), db.raw(), &k, &v, 0), "mdb_put");

        wtxn.commit();
    }

    // Read
    {
        lmdb::Txn rtxn(env, lmdb::Txn::Mode::ReadOnly);
        auto db = lmdb::Dbi::open(rtxn, "main");

        MDB_val k{3, (void*)"key"};
        MDB_val v{};
        int rc = mdb_get(rtxn.raw(), db.raw(), &k, &v);
        if (rc == MDB_SUCCESS) {
            std::cout << std::string_view((char*)v.mv_data, v.mv_size) << "\n";
        } else if (rc != MDB_NOTFOUND) {
            lmdb::throw_on(rc, "mdb_get");
        }
    }
    return 0;
}