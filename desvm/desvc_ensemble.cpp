#include "../mdt_core/count_line.hpp"
#include "../mdt_core/in_range.hpp"

#include "../ssvm/LinearSSVC.hpp"
#include "../ssvm/RBFRSVC.hpp"
#include "../ssvm/SparseData.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <boost/mpi.hpp>

using namespace mdt_core;
using namespace ssvm;
using namespace std;

namespace mpi = boost::mpi;

int main(int argc, char *argv[]) {
  try {
    if (argc != 13) {
      throw runtime_error("<file root> <data folder> <model name> "
                          "<split number> <feature dimension> "
                          "<pos name> <neg name> "
                          "<final weight> "
                          "<loading number> <sample A name> <sample y name> <sample number>");
    }
    mpi::environment env(argc, argv);
    mpi::communicator world;
    if (!world) {
      throw runtime_error("communicator fail");
    }
    const Integer worker_number(world.size() - 1);
    if (worker_number < 1) {
      throw runtime_error("worker_number must > 1");
    }
    const string root(argv[1] + string("/") + argv[2]);
    const string model(argv[3]);
    const Integer split(in_range(integer_from_str(argv[4]),
                                 1, numeric_limits<Integer>::max()));
    const Integer dimension(in_range(integer_from_str(argv[5]),
                                     1, numeric_limits<Integer>::max()));
    const string positive_name(argv[6]);
    const string negative_name(argv[7]);
    const Number final_weight(in_range(number_from_str(argv[8]),
                                       numeric_limits<Number>::epsilon(), numeric_limits<Number>::max()));
    const Integer loading_number(in_range(integer_from_str(argv[9]),
                                          1, numeric_limits<Integer>::max()));
    const string sample_pos_name(argv[10]);
    const string sample_neg_name(argv[11]);
    const Integer sample_number(in_range(integer_from_str(argv[12]),
                                          1, numeric_limits<Integer>::max()));
    if (worker_number - loading_number < 0) {
      throw runtime_error("too much loading_number");
    }
    if (world.rank() == worker_number) {
      clock_t start(clock());

      try {
        Integer positive_number(0), negative_number(0);
        for (Integer i(0); i < sample_number; ++i) {
          stringstream sample_pos_path;
          sample_pos_path << root << '/' << split << '/' << i << '/' << sample_pos_name;
          positive_number += count_line(sample_pos_path.str().c_str());
          stringstream sample_neg_path;
          sample_neg_path << root << '/' << split << '/' << i << '/' << sample_neg_name;
          negative_number += count_line(sample_pos_path.str().c_str());
        }
        const Integer instance_number(positive_number + negative_number);
        Matrix final_table(instance_number, split + 1);
        Matrix support_vector(instance_number, split + 1);

        Integer task_id(0);
        vector<RBFRSVC> submodels(split);
        vector<Integer> loading2task(loading_number, -1);
        vector<mpi::request> submodel_reqs(loading_number);
        while (true) {
          Integer task_done(0);
          for (Integer i(0); i < loading_number; ++i) {
            if (loading2task[i] == split) {
              ++task_done;
              continue;
            }
            if (loading2task[i] != -1 && submodel_reqs[i].test()) {
              loading2task[i] = -1;
            }
            if (loading2task[i] == -1) {
              loading2task[i] = task_id;
              mpi::request task_id_req(world.isend(i + (worker_number - loading_number), 0, loading2task[i]));
              task_id_req.wait();
              if (task_id < split) {
                submodel_reqs[i] = world.irecv(i + (worker_number - loading_number), 1, submodels[loading2task[i]]);
                ++task_id;
              }
            }
          }
          if (task_done == loading_number) {
            break;
          }
        }
        loading2task = vector<Integer>();
        submodel_reqs = vector<mpi::request>();

        task_id = 0;
        vector<Number> subpredict;
        vector<Integer> worker2task(worker_number, -1);
        vector<mpi::request> subpredict_reqs(worker_number);
        while (true) {
          Integer task_done(0);
          for (Integer i(0); i < worker_number; ++i) {
            if (worker2task[i] == split) {
              ++task_done;
              continue;
            }
            if (worker2task[i] != -1 && subpredict_reqs[i].test()) {
              Number *dst(final_table.data() + worker2task[i] * instance_number);
              Number *src(subpredict.data());
              for (Integer j(0); j < instance_number; ++j, ++dst, ++src) {
                *dst = *src;
              }
              worker2task[i] = -1;
            }
            if (worker2task[i] == -1) {
              worker2task[i] = task_id;
              mpi::request task_id_req(world.isend(i, 0, worker2task[i]));
              task_id_req.wait();
              if (task_id < split) {
                mpi::request submodel_req(world.isend(i, 1, submodels[worker2task[i]]));
                submodel_req.wait();
                subpredict_reqs[i] = world.irecv(i, 2, subpredict);
                ++task_id;
              }
            }
          }
          if (task_done == worker_number) {
            break;
          }
        }
        subpredict = vector<Number>();

        LinearSSVC final_model(negative_number, final_weight, &final_table, &support_vector);
        stringstream final_model_path;
        final_model_path << root << '/' << split << '/' << model;
        final_model.dump(final_model_path.str());
      } catch (const exception& e) {
        cout << '<' << world.rank() << '>' << e.what() << endl;
        throw;
      }

      cout << (double)(clock() - start) / CLOCKS_PER_SEC;
    } else {
      try {
        Integer task_id(-1);
        while (world.rank() >= (worker_number - loading_number)) {
          mpi::request task_id_req(world.irecv(worker_number, 0, task_id));
          task_id_req.wait();
          if (task_id == split) {
            break;
          }
          stringstream submodel_path;
          submodel_path << root << '/' << split << '/' << task_id << '/' << model;
          RBFRSVC submodel(submodel_path.str().c_str());
          mpi::request submodel_req(world.isend(worker_number, 1, submodel));
          submodel_req.wait();
        }

        task_id = -1;
        RBFRSVC submodel;
        vector<SparseMatrix> Ps(sample_number), Ns(sample_number);
        for (Integer i(0); i < sample_number; ++i) {
          stringstream pos_path;
          pos_path << root << '/' << split << '/' << i << '/' << sample_pos_name;
          SparseData::read(pos_path.str().c_str(), dimension, &(Ps[i]));

          stringstream neg_path;
          neg_path << root << '/' << split << '/' << i << '/' << sample_neg_name;
          SparseData::read(neg_path.str().c_str(), dimension, &(Ns[i]));
        }

        Matrix kernel_data;
        Vector result;
        vector<Number> subpredict;
        while (true) {
          mpi::request task_id_req(world.irecv(worker_number, 0, task_id));
          task_id_req.wait();
          if (task_id == split) {
            break;
          }
          mpi::request submodel_req(world.irecv(worker_number, 1, submodel));
          submodel_req.wait();
          subpredict.clear();
          for (Integer i(0); i < sample_number; ++i) {
            submodel.predict(Ps[i], &kernel_data, &result);
            for (Integer k(0); k < result.size(); ++k) {
              subpredict.push_back(result(k));
            }
            submodel.predict(Ns[i], &kernel_data, &result);
            for (Integer k(0); k < result.size(); ++k) {
              subpredict.push_back(result(k));
            }
          }
          mpi::request subpredict_req(world.isend(worker_number, 2, subpredict));
          subpredict_req.wait();
        }
        submodel = RBFRSVC();
        Ps = vector<SparseMatrix>();
        Ns = vector<SparseMatrix>();
        kernel_data = Matrix();
        result = Vector();
        subpredict = vector<Number>();
      
      } catch (const exception& e) {
        cout << '<' << world.rank() << '>' << e.what() << endl;
        throw;
      }
    }
    return EXIT_SUCCESS;
  } catch (const exception& e) {
    cout << "desvc_ensemble : \n" << e.what() << endl;
    return EXIT_FAILURE;
  }
}
