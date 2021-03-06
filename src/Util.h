#ifndef __NNDEP_UTIL_H__
#define __NNDEP_UTIL_H__

#include <vector>
#include <cstdlib>
// #include <ctime>
#include <iostream>
#include <fstream>
// #include <map>
#include <unordered_map>
#include <cmath>

#include "DependencySent.h"
#include "DependencyTree.h"
#include "DependencyGraph.h"
#include "Config.h"
#include "strutils.h"
#include "math/mat.h"

class Util
{
    private:
        Util()
        {
        } // static methods

    public:
        template <typename T>
        static std::vector<T> get_random_subset(
                std::vector<T>& input,
                int subset_size)
        {
            int input_size = input.size();
            if (subset_size > input_size)
                subset_size = input_size;

            for (int i = 0; i < subset_size; ++i)
            {
                // size_t i_swap = i + rand() % (input_size - i);
                size_t i_swap = rand_int(i, input_size - i);
                T temp = input[i];
                input[i] = input[i_swap];
                input[i_swap] = temp;
            }

            auto beg = input.begin();
            auto end = input.begin() + subset_size;
            std::vector<T> subset(beg, end);
            return subset;
        }

        template <typename T>
        static void get_minibatch(
                std::vector<T>& input,
                std::vector<T>& output,
                int minibatch_size,
                int cursor)
        {
            /**
             * There will be two parts of sub-vectors
             *  - vector1: cursor -> end_pos1
             *  - vector2: begin() -> (minibatch_size - (end_pos1 - cursor))
             */
            output.clear();

            int input_size = input.size();
            if (minibatch_size > input_size)
            {
                output = input;
                return ;
            }

            int end_pos1 = cursor + minibatch_size;
            int end_pos2 = 0;
            if (end_pos1 > input_size)
            {
                end_pos1 = input_size;
                end_pos2 = minibatch_size - (end_pos1 - cursor);
            }

            auto beg1 = input.begin() + cursor;
            auto end1 = input.begin() + end_pos1;
            std::vector<T> subvec1 = std::vector<T>(beg1, end1);

            auto beg2 = input.begin();
            auto end2 = input.begin() + end_pos2;
            std::vector<T> subvec2 = std::vector<T>(beg2, end2);

            output.reserve(subvec1.size() + subvec2.size());
            output.insert(output.end(), subvec1.begin(), subvec1.end());
            if (end_pos2 > 0)
                output.insert(output.end(), subvec2.begin(), subvec2.end());
        }

        template <typename T>
        static void partition_into_chunks(
                std::vector<T> & input,
                std::vector< std::vector<T>> & output,
                int num_chunks)
        {
            output.clear();
            int chunk_size = input.size() / num_chunks;
            int remainder = input.size() % num_chunks;

            int start = 0;
            int end = chunk_size;
            for (int i = 0; i < num_chunks; ++i)
            {
                if (i < remainder)
                    end ++;

                auto l = input.begin() + start;
                auto r = input.begin() + end;
                output.push_back(std::vector<T>(l, r));

                start = end;
                end += chunk_size;
            }
        }

        /**
         * randomly generate uniform-distributed numbers
         */
        static int rand_int(int max)
        {
            return rand() % max;
        }

        static int rand_int(int min, int max)
        {
            return min + rand_int(max);
        }

        static double rand_double()
        {
            return (double)rand() / (double)RAND_MAX;
        }

        static void load_conll_file_graph(
                const char * file,
                std::vector<DependencySent>& sents,
                std::vector<DependencyGraph>& graphs,
                bool labeled)
        {
            std::ifstream conll_reader(file);
            int non_local_arc_cnt = 0;
            if (conll_reader.fail())
            {
                std::cerr << "# fail to open conll file: "
                          << file
                          << std::endl;
                return ;
            }

            DependencySent sent;
            DependencyGraph graph;
            std::string line;

            int i = 0;
            while (getline(conll_reader, line))
            {
                std::cerr << "\r" << i++;
                // getline(conll_reader, line);
                std::vector<std::string> sep = split(line);
                if (sep.size() < 10) // end of a sentence
                {
                   // tree.cal_projective_order_and_mpc(); // better to put somewhere else
                    sents.push_back(sent);
                    graphs.push_back(graph);

                    sent.init();
                    graph.init();
                }
                else
                {
                    int id = to_int(sep[0]);
                    std::string word   = sep[1];
                    std::string pos    = sep[3];
                    // std::string pos   = sep[4];
                    std::string cluster = sep[5];
                    std::string deprel = sep[7];
                    int head = to_int(sep[6]);
                    // std::string deprel = sep[9];
                    // int head = to_int(sep[8]);
                    //std::cerr << id << std::endl;
                    if (id == sent.n+1){
                        sent.add(word, pos, cluster);
                        std::vector<int> h;
                        std::vector<std::string> l;
                        h.push_back(head);
                        if (labeled){
                            l.push_back(deprel);
                            graph.add(h, l);
                        }
                        else{ // currently unused
                            l.push_back(Config::UNKNOWN);
                            graph.add(h, l);
                        }
                    }
                    else if (id == sent.n){
                        non_local_arc_cnt ++;
                        if (labeled)
                            graph.set(id, head, deprel);
                        else // currently unused
                            graph.set(id, head, Config::UNKNOWN);
                    }
                    else{
                        std::cerr << "# error loading graph!"<<std::endl;
                    }
                    
                }
            }
            std::cerr << std::endl;
            conll_reader.close();
            std::cerr << "sentences number:" << sents.size() << "  non-local arc number:" << non_local_arc_cnt << std::endl;
        }

        static void load_conll_file_graph(
                const char * file,
                std::vector<DependencySent>& sents,
                std::vector<DependencyGraph>& graphs)
        {
            load_conll_file_graph(file, sents, graphs, true);
        }

        static void load_conll_file(
                const char * file,
                std::vector<DependencySent>& sents,
                std::vector<DependencyTree>& trees,
                bool labeled)
        {
            std::ifstream conll_reader(file);
            if (conll_reader.fail())
            {
                std::cerr << "# fail to open conll file: "
                          << file
                          << std::endl;
                return ;
            }

            DependencySent sent;
            DependencyTree tree;
            std::string line;
            int i = 0;
            while (getline(conll_reader, line))
            {
                std::cerr << "\r" << i++;
                // getline(conll_reader, line);
                std::vector<std::string> sep = split(line);
                if (sep.size() < 10) // end of a sentence
                {
                    tree.cal_projective_order_and_mpc(); // better to put somewhere else
                    sents.push_back(sent);
                    trees.push_back(tree);

                    sent.init();
                    tree.init();
                }
                else
                {
                    std::string word   = sep[1];
                    std::string pos    = sep[3];
                    // std::string pos   = sep[4];
                    std::string cluster = sep[5];
                    std::string deprel = sep[7];
                    int head = to_int(sep[6]);
                    // std::string deprel = sep[9];
                    // int head = to_int(sep[8]);

                    sent.add(word, pos, cluster);

                    if (labeled)
                        tree.add(head, deprel);
                    else // currently unused
                        tree.add(head, Config::UNKNOWN);
                }
            }
            std::cerr << std::endl;
            conll_reader.close();
        }

        static void load_conll_file(
                const char * file,
                std::vector<DependencySent>& sents,
                std::vector<DependencyTree>& trees)
        {
            load_conll_file(file, sents, trees, true);
        }

        static void write_conll_file_graph(
                const char * file,
                std::vector<DependencySent>& sents,
                std::vector<DependencyGraph>& graphs)
        {
            std::ofstream conll_writer(file);
            
            for (size_t i = 0; i < sents.size(); ++i)
            {
                for (int j = 0; j < sents[i].n; ++j){//node j
                    std::vector<int> head = graphs[i].get_head(j+1);
                    for (int k = 0; k < head.size(); ++k){
                        conll_writer << j + 1 << "\t"
                                 << sents[i].words[j] << "\t_\t"
                                 << sents[i].poss[j] << "\t_\t_\t"
                                 << head[k] << "\t"
                                 << graphs[i].get_arc_label(j + 1, head[k]) << "\t"
                                 << "_\t_\n";
                    }
                }
                conll_writer << "\n";
            }

            conll_writer.close();
        }

        static void write_conll_file(
                const char * file,
                std::vector<DependencySent>& sents,
                std::vector<DependencyTree>& trees)
        {
            std::ofstream conll_writer(file);

            for (size_t i = 0; i < sents.size(); ++i)
            {
                for (int j = 0; j < sents[i].n; ++j)
                    conll_writer << j + 1 << "\t"
                                 << sents[i].words[j] << "\t_\t"
                                 << sents[i].poss[j] << "\t_\t_\t"
                                 << trees[i].get_head(j + 1) << "\t"
                                 << trees[i].get_label(j + 1) << "\t"
                                 << "_\t_\n";
                conll_writer << "\n";
            }

            conll_writer.close();
        }

        static void load_mtl_files(
                const char * mtl_file,
                std::map<int, std::string> & files)
        {
            files.clear();

            std::ifstream mtl_reader(mtl_file);
            if (mtl_reader.fail())
            {
                std::cerr << "# fail to open mtl file: "
                          << mtl_file
                          << std::endl;
                return ;
            }

            std::string line;
            int i = 0;
            while (getline(mtl_reader, line))
            {
                line = cutoff(line, "#");
                if (line.size() == 0) continue;

                std::vector<std::string> sep = split_by_sep(line, ":");
                if (sep.size() < 2)
                    std::cerr << "Invalid line in "
                              << mtl_file
                              << ": "
                              << i
                              << std::endl;
                int task_id = to_int(sep[0]);
                std::string path = sep[1];
                files[task_id] = path;
            }

            mtl_reader.close();
        }

        static void load_mtl_files(
                const char * mtl_file,
                std::map<int, std::pair<std::string, std::string>> & files)
        {
            files.clear();

            std::ifstream mtl_reader(mtl_file);
            if (mtl_reader.fail())
            {
                std::cerr << "# fail to open mtl file: "
                          << mtl_file
                          << std::endl;
                return ;
            }

            std::string line;
            int i = 0;
            while (getline(mtl_reader, line))
            {
                line = cutoff(line, "#");
                if (line.size() == 0) continue;

                std::vector<std::string> sep = split_by_sep(line, ":");
                if (sep.size() < 3)
                    std::cerr << "Invalid line in "
                              << mtl_file
                              << ": "
                              << i
                              << std::endl;
                int task_id = to_int(sep[0]);
                std::string path1 = sep[1];
                std::string path2 = sep[2];
                files[task_id] = std::make_pair(path1, path2);
            }

            mtl_reader.close();
        }

        static void load_mtl_config(
                const char * mtl_file,
                std::map<int, std::map<std::string, std::string>> & mtl_cfg)
        {
            mtl_cfg.clear();

            std::ifstream mtl_reader(mtl_file);
            if (mtl_reader.fail())
            {
                std::cerr << "# fail to open mtl file: "
                          << mtl_file
                          << std::endl;
                return ;
            }

            std::string line;
            int i = 0;
            while (getline(mtl_reader, line))
            {
                line = cutoff(line, "#");
                if (line.size() == 0) continue;
                std::vector<std::string> sep = split_by_sep(line, ":");
                if (sep.size() < 3)
                    std::cerr << "Invalid line in "
                              << mtl_file
                              << ": "
                              << i
                              << std::endl;
                int task_id = to_int(sep[0]);
                std::map<std::string, std::string> cfg;
                cfg["path"] = sep[1];
                cfg["main"] = sep[2];
                mtl_cfg.insert(make_pair(task_id, cfg));
            }
            mtl_reader.close();
        }

        static void print_tree_stats(std::vector<DependencyTree>& trees)
        {
            std::cerr << Config::SEPERATOR << std::endl;
            std::cerr << "#Trees: " << trees.size() << std::endl;

            int non_trees = 0;
            int non_projectives = 0;

            for (size_t i = 0; i < trees.size(); ++i)
            {
                if (!trees[i].is_tree())
                    ++ non_trees;
                else if (!trees[i].is_projective())
                    ++ non_projectives;
            }

            std::cerr << non_trees
                      << " tree(s) are illegal"
                      << std::endl;
            std::cerr << non_projectives
                      << " tree(s) are legal but not projective"
                      << std::endl;
        }

        template <typename T>
        static std::vector<T> generate_dict(
                std::vector<T>& tok_set,
                int cutoff)
        {
            std::unordered_map<T, int> tok_freq;
            for (size_t i = 0; i < tok_set.size(); ++i)
            {
                if (tok_freq.find(tok_set[i]) == tok_freq.end())
                    tok_freq[tok_set[i]] = 1;
                else
                    tok_freq[tok_set[i]] += 1;
            }

            std::vector<T> dict;
            typename std::unordered_map<T, int>::iterator iter;
            for (iter = tok_freq.begin();
                    iter != tok_freq.end();
                    ++iter)
            {
                if (iter->second >= cutoff)
                    dict.push_back(iter->first);
            }

            return dict;
        }

        template <typename T>
        static std::vector<T> generate_dict(
                std::vector<T>& tok_set)
        {
            return generate_dict(tok_set, 1);
        }

        template <typename K, typename V>
        static bool comp_by_value_ascending(
                std::pair<K, V> m1,
                std::pair<K, V> m2)
        {
            return m1.second < m2.second;
        }

        template <typename K, typename V>
        static bool comp_by_value_descending(
                std::pair<K, V> m1,
                std::pair<K, V> m2)
        {
            return m1.second > m2.second;
        }

        template <typename T>
        static void mat_demo(const Mat<T> & m, const std::string & msg)
        {
            std::cerr << "*** " << msg << " ***" << std::endl;
            // std::cerr << "[";
            for (int i = 0; i < m.nrows(); ++i)
            {
                std::cerr << "[" << m[i][0];
                for (int j = 1; j < m.ncols(); ++j)
                    std::cerr << " " << m[i][j];
                std::cerr << "]" << std::endl;
            }
            // std::cerr << "]" << std::endl;
        }

        template <typename T>
        static void vec_demo(const Vec<T> & m, const std::string & msg)
        {
            std::cerr << "*" << msg << "*" << std::endl;
            std::cerr << "[";
            std::cerr << m[0];
            for (int i = 1; i < m.nrows(); ++i)
                std::cerr << " " << m[i];
            std::cerr << "]" << std::endl;
        }

        template <typename T>
        static Mat<T> mat_subtract(const Mat<T> & mat1, const Mat<T> & mat2)
        {
            Mat<T> result(mat1.nrows(), mat1.ncols());
            for (int i = 0; i < mat1.nrows(); ++i)
                for (int j = 0; j < mat1.ncols(); ++j)
                    result[i][j] = mat1[i][j] - mat2[i][j];
            return result;
        }

        template <typename T>
        static Mat3<T> mat3_subtract(const Mat3<T> & mat1, const Mat3<T> & mat2)
        {
            Mat3<T> result(mat1.dim1(), mat1.dim2(), mat1.dim3());
            for (int i = 0; i < mat1.dim1(); ++i)
                for (int j = 0; j < mat1.dim2(); ++j)
                    for (int k = 0; k < mat1.dim3(); ++k)
                        result[i][j][k] = mat1[i][j][k] - mat2[i][j][k];
            return result;
        }

        template <typename T>
        static Mat<T> mat_add(const Mat<T> & mat1, const Mat<T> & mat2)
        {
            Mat<T> result(mat1.nrows(), mat1.ncols());
            for (int i = 0; i < mat1.nrows(); ++i)
                for (int j = 0; j < mat1.ncols(); ++j)
                    result[i][j] = mat1[i][j] + mat2[i][j];
            return result;
        }

        template <typename T>
        static Mat3<T> mat3_add(const Mat3<T> & mat1, const Mat3<T> & mat2)
        {
            Mat3<T> result(mat1.dim1(), mat1.dim2(), mat1.dim3());
            for (int i = 0; i < mat1.dim1(); ++i)
                for (int j = 0; j < mat1.dim2(); ++j)
                    for (int k = 0; k < mat1.dim3(); ++k)
                        result[i][j][k] = mat1[i][j][k] + mat2[i][j][k];
            return result;
        }

        template <typename T>
        static Vec<T> vec_subtract(const Vec<T> & vec1, const Vec<T> & vec2)
        {
            Vec<T> result(vec1.size());
            for (int i = 0; i < vec1.size(); ++i)
                    result[i] = vec1[i] - vec2[i];
            return result;
        }

        template <typename T>
        static Vec<T> vec_add(Vec<T> & vec1, const Vec<T> & vec2)
        {
            Vec<T> result(vec1.size());
            for (int i = 0; i < vec1.size(); ++i)
                    result[i] = vec1[i] + vec2[i];
            return result;
        }

        template <typename T>
        static void vec_inc(Vec<T> & vec1, const Vec<T> & vec2)
        {
            for (int i = 0; i < vec1.size(); ++i)
                    vec1[i] += vec2[i];
        }

        template <typename T>
        static void mat_inc(Mat<T> & mat1, const Mat<T> & mat2)
        {
            for (int i = 0; i < mat1.nrows(); ++i)
                for (int j = 0; j < mat1.ncols(); ++j)
                    mat1[i][j] += mat2[i][j];
        }

        template <typename T>
        static void mat3_inc(Mat3<T> & mat1, const Mat3<T> & mat2)
        {
            for (int i = 0; i < mat1.dim1(); ++i)
                for (int j = 0; j < mat1.dim2(); ++j)
                    for (int k = 0; k < mat1.dim3(); ++k)
                        mat1[i][j][k] += mat2[i][j][k];
        }

        template <typename T>
        static double l2_norm(const Mat<T> & mat)
        {
            double result = 0.0;
            for (int i = 0; i < mat.nrows(); ++i)
                for (int j = 0; j < mat.ncols(); ++j)
                    result += mat[i][j] * mat[i][j];
            return sqrt(result);
        }

        template <typename T>
        static double l2_norm(const Mat3<T> & mat)
        {
            double result = 0.0;
            for (int i = 0; i < mat.dim1(); ++i)
                for (int j = 0; j < mat.dim2(); ++j)
                    for (int k = 0; k < mat.dim3(); ++k)
                        result += mat[i][j][k] * mat[i][j][k];
            return sqrt(result);
        }

        template <typename T>
        static double l2_norm(const Vec<T> & vec)
        {
            double result = 0.0;
            for (int i = 0; i < vec.size(); ++i)
                    result += vec[i] * vec[i];
            return sqrt(result);
        }
};

#endif

