#include <iostream>
#include <fstream>
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cmath>
#include <cassert>

//#include "ArcStandard.h"
//#include "NonProjNivre.h"
//#include "AttachSystem.h"
#include "ArcEager.h"
#include "ListSystem.h"
#include "DependencyParser.h"
#include "Util.h"
#include "Config.h"
#include "time.h"

#include <omp.h>
// #include "../utils/io.h"
// #include "../utils/logging.h"

using namespace std;

DependencyParser::DependencyParser(const char * cfg_filename)
{
    config.set_properties(cfg_filename);
}

DependencyParser::DependencyParser(string& cfg_filename)
{
    config.set_properties(cfg_filename.c_str());
}

DependencyParser::~DependencyParser()
{
    system = NULL; delete system;
    classifier = NULL; delete classifier;
    word_ids.clear();
    pos_ids.clear();
    label_ids.clear();
    distance_ids.clear();
    valency_ids.clear();
    cluster_ids.clear();
    length_ids.clear();
}

void DependencyParser::train(
        string & train_file,
        string & dev_file,
        string & model_file,
        string & embed_file,
        string & premodel_file,
        int sub_sampling)
{
    train(train_file.c_str(),
            dev_file.c_str(),
            model_file.c_str(),
            embed_file.c_str(),
            premodel_file.c_str(),
            sub_sampling);
}

void DependencyParser::train(
        const char * train_file,
        const char * dev_file,
        const char * model_file,
        const char * embed_file,
        const char * premodel_file,
        int sub_sampling)
{
    // omp_set_num_threads(30);
    omp_set_num_threads(6);
    int n_threads = omp_get_max_threads();
    if (n_threads > 1)
        cerr << "Using " << n_threads << " threads" << endl;

    cerr << "Train file:     " << train_file << endl;
    cerr << "Dev file:       " << dev_file   << endl;
    cerr << "Model file:     " << model_file << endl;
    cerr << "Embedding file: " << embed_file << endl;

    /**
     * collect training instances from train_file
     */
    //vector<DependencyTree> train_trees;
    vector<DependencyGraph> train_graphs;
    vector<DependencySent> train_sents;

    cerr << "Loading training file (conll)" << endl;
    Util::load_conll_file_graph(train_file, train_sents, train_graphs, config.labeled);
    if (sub_sampling != -1 && (unsigned)sub_sampling < train_sents.size())
    {
        // vector<DependencySent>::const_iterator s_beg = train_sents.begin();
        // vector<DependencySent>::const_iterator s_end = train_sents.begin() + sub_sampling;
        auto s_beg = train_sents.begin();
        auto s_end = train_sents.begin() + sub_sampling;
        train_sents = vector<DependencySent>(s_beg, s_end);

        // vector<DependencyTree>::const_iterator t_beg = train_trees.begin();
        // vector<DependencyTree>::const_iterator t_end = train_trees.begin() + sub_sampling;
        auto t_beg = train_graphs.begin();
        auto t_end = train_graphs.begin() + sub_sampling;
        train_graphs = vector<DependencyGraph>(t_beg, t_end);

        cerr << "Sub-sampling " << sub_sampling << " sentences/trees for training." << endl;
    }

    //Util::print_tree_stats(train_trees); //to be done

    //vector<DependencyTree> dev_trees;
    vector<DependencyGraph> dev_graphs;
    vector<DependencySent> dev_sents;
    if (dev_file[0] != 0)
    {
        cerr << "Loading devel file (conll)" << endl;
        Util::load_conll_file_graph(dev_file, dev_sents, dev_graphs, config.labeled);
        //Util::print_tree_stats(dev_trees); //to be done
    }


    // Read embedding file and initialize the embedding matrix
    cerr << "Reading word embeddings" << endl;
    read_embed_file(embed_file);


    /**
     * fill @known_words, @known_poss, @known_labels
     */
    cerr << "Generating dictionaries" << endl;
    gen_dictionaries_graph(train_sents, train_graphs);

    // TODO
    vector<string> ldict = known_labels;
    if (config.labeled) ldict.pop_back(); // remove the NIL label
    if (config.oracle == "arceager")
        system =  new ArcEager(ldict, config.language, config.labeled);
    else if (config.oracle == "listsystem")
        system =  new ListSystem(ldict, config.language, config.labeled);

    cerr << "Setup classifier for training" << endl;
    setup_classifier_for_training(train_sents, train_graphs, embed_file, premodel_file);
    config.print_info();

    /**
     * Gradient Check
     */
    if (config.debug)
    {
        classifier->check_gradient();
        save_model(string(model_file)); // can rename
        return ;
    }

    save_model(string(model_file) + ".0"); // initial model

    //double best_uas = -DBL_MAX;
    double best_lf = -DBL_MAX;
    for (int iter = 1; iter <= config.max_iter; ++iter)
    {
        /**
         * Compute current cost
         * and all gradients
         */
        double before = get_time();
        classifier->compute_cost_function();
        double after = get_time();
        // double cost = classifier->get_cost();
        cerr << "#Iteration " << iter << ": "
             << "Cost = " << classifier->get_loss()
             << ", Correct(%) = " << classifier->get_accuracy()
             << " (" << (after - before) << ")"
             << endl;
        /**
         * update gradient using AdaGrad
         */
        // fix all words, except -UNKNOWN-, -NULL-, and -ROOT-
        if (config.fix_word_embeddings && !config.delexicalized)
            classifier->take_ada_gradient_step(known_words.size() - 3);
        else
            classifier->take_ada_gradient_step();

        if (dev_file[0] != 0 && iter % config.eval_per_iter == 0)
        {
            classifier->pre_compute(); // with updated weights
            vector<DependencyGraph> predicted;
            predict_graph(dev_sents, predicted);

            map<string, double> result;
            system->evaluate(dev_sents, predicted, dev_graphs, result);
            //double uas = result["UASwoPunc"];
            //double las = result["LASwoPunc"];
            //double uem = result["UEMwoPunc"];
            double root = result["ROOT"];

            double LF = result["LF"];
            double UF = result["UF"];
            //double NUF = result["NUF"];
            //double NLF = result["NLF"];

            cerr << "UAS(dev)=" << UF << "%" << endl;
            cerr << "LAS(dev)=" << LF << "%" << endl;
            //cerr << "NUF(dev)=" << NUF << "%" << endl;
            //cerr << "NLF(dev)=" << NLF << "%" << endl;
            //cerr << "UAS(dev) = " << uas << "%" << endl;
            //cerr << "LAS(dev) = " << las << "%" << endl;
            //cerr << "UEM(dev) = " << uem << "%" << endl;
            cerr << "ROOT(dev) = " << root << "%" << endl;

            if (config.save_intermediate && LF > best_lf)
            {
                best_lf = LF;
                // save_model(string(model_file) + "." + to_str(iter)); // can rename
                cerr << "saving model to:" << model_file << endl;
                save_model(string(model_file)); // can rename
            }
        }

        if (iter % (10 * config.eval_per_iter) == 0)
            save_model(string(model_file) + "." + to_str(iter));

        if (config.clear_gradient_per_iter > 0
                && iter % config.clear_gradient_per_iter == 0)
        {
            classifier->clear_gradient_histories();
        }
    }

    classifier->finalize_training();

    if (dev_file[0] != 0)
    {
        vector<DependencyGraph> predicted;
        predict_graph(dev_sents, predicted);
        // get_uas_scoredouble uas = system->get_uas_score(dev_sents, predicted, dev_trees);

        map<string, double> result;
        system->evaluate(dev_sents, predicted, dev_graphs, result);
        //double uas = result["UASwoPunc"];
        //double las = result["LASwoPunc"];
        //double uem = result["UEMwoPunc"];
        double root = result["ROOT"];

        double LF = result["LF"];
        double UF = result["UF"];
        double NUF = result["NUF"];
        double NLF = result["NLF"];

        cerr << "Final model: " << endl;
        cerr << "UF(dev)=" << UF << "%" << endl;
        cerr << "LF(dev)=" << LF << "%" << endl;
        cerr << "NUF(dev)=" << NUF << "%" << endl;
        cerr << "NLF(dev)=" << NLF << "%" << endl;
        cerr << "Best model: " << endl
             << "\tLF(Best) = " << best_lf << endl;

        /*
        cerr << "Final model: " << endl
             << "\tUAS = " << uas << endl
             << "\tLAS = " << las << endl
             << "\tUEM = " << uem << endl
             << "\tROOT = " << root << endl;
        cerr << "Best model: " << endl
             << "\tUAS(Best) = " << best_uas << endl;*/

        if (LF > best_lf)
        {
            cerr << "saving model to:" << model_file << endl;
            save_model(model_file);
        }
    }
    else
    {
        save_model(model_file);
    }
}

void DependencyParser::finetune(
                const char * train_file, // target language
                const char * premodel_file,
                const char * model_file,
                const char * emb_file,
                int sub_sampling)
{
    omp_set_num_threads(config.training_threads);
    int n_threads = omp_get_max_threads();
    if (n_threads > 1)
        cerr << "Using " << n_threads << " threads" << endl;

    cerr << "Finetuning model:      " << premodel_file << endl;
    cerr << "Training file:         " << train_file << endl;
    cerr << "Target embedding file: " << emb_file   << endl;

    // load training data from target language
    vector<DependencyGraph> train_graphs;
    vector<DependencySent> train_sents;

    cerr << "Loading training file (conll) for finetuning" << endl;
    Util::load_conll_file_graph(train_file, train_sents, train_graphs, config.labeled);
    if (sub_sampling != -1 && (unsigned)sub_sampling < train_sents.size())
    {
        // vector<DependencySent>::const_iterator s_beg = train_sents.begin();
        // vector<DependencySent>::const_iterator s_end = train_sents.begin() + sub_sampling;
        auto s_beg = train_sents.begin();
        auto s_end = train_sents.begin() + sub_sampling;
        train_sents = vector<DependencySent>(s_beg, s_end);

        // vector<DependencyTree>::const_iterator t_beg = train_trees.begin();
        // vector<DependencyTree>::const_iterator t_end = train_trees.begin() + sub_sampling;
        auto t_beg = train_graphs.begin();
        auto t_end = train_graphs.begin() + sub_sampling;
        train_graphs = vector<DependencyGraph>(t_beg, t_end);
    }

    cerr << "Sub-sampling " << sub_sampling << " sentences/trees for finetuning." << endl;

    //Util::print_tree_stats(train_trees);
    // Attention: no dev trees are used here

    // gen_dictionaries(train_sents, train_trees);

    // Load model, ignore word embeddings (n_dict) from source language
    // instead, fill with word embeddings from target language.
    //  - NB: keep the three tokens from SL (ROOT, UNKNOWN, NULL).
    //
    // Besides, fix_word_embeddings should be true
    cerr << "Load model trained from source language." << endl;
    if (config.delexicalized)   load_model(premodel_file);
    else                        load_model_cl(premodel_file, emb_file);

    Dataset dataset = gen_train_samples_graph(train_sents, train_graphs);
    // classifier = new NNClassifier(config, dataset, Eb, Ed, Ev, Ec, W1, b1, W2, pre_computed_ids);
    // if (classifier) delete classifier;
    classifier->set_dataset(dataset, pre_computed_ids);
    config.print_info();

    // fine-tuning
    assert (config.fix_word_embeddings == true);
    for (int iter = 1; iter <= config.finetune_iter; ++iter)
    {
        double before = get_time();
        classifier->compute_cost_function();
        double after = get_time();
        cerr << "#Iteration " << (iter + 1) << ": "
             << "Cost = " << classifier->get_loss()
             << ", Correct(%) = " << classifier->get_accuracy()
             << " (" << (after - before) << ")"
             << endl;

        classifier->take_ada_gradient_step(known_words.size() - 3);
        if (iter % 50 == 0)
            save_model(string(model_file) + "." + to_str(iter));
    }

    // string finetuned_model_path = string(model_file) + ".finetuned." + to_str(sub_sampling);
    cerr << "Saved model to " << model_file << endl;
    save_model(model_file);
}

void DependencyParser::finetune(
                string & train_file, // target language
                string & premodel_file,
                string & model_file,
                string & emb_file,
                int sub_sampling)
{
    finetune(train_file.c_str(),
             premodel_file.c_str(),
             model_file.c_str(),
             emb_file.c_str(),
             sub_sampling);
}

void DependencyParser::extract_transition_sequence(const char * train_file, const char * oracle_file)
{
    vector<DependencyGraph> graphs;
    vector<DependencySent> sents;
    std::ofstream oracle_writer(oracle_file);
    cerr << "extracting transition sequences to file "<<oracle_file<<endl;
    Util::load_conll_file_graph(train_file, sents, graphs, config.labeled);

    gen_dictionaries_graph(sents, graphs);

    vector<string> ldict = known_labels;
    ldict.pop_back();
    ParsingSystem * monitor;
    if (config.oracle == "arceager")
        monitor =  new ArcEager(ldict, config.language, config.labeled);
    else if (config.oracle == "listsystem")
        monitor =  new ListSystem(ldict, config.language, config.labeled);
    int error_cnt = 0;
    for (size_t i = 0; i < sents.size(); ++i)
    {
        //cout << "[" << i << "]";
        //oracle_writer << "[" << i << "]" << endl;
        oracle_writer << endl;
        Configuration c(sents[i]);
        oracle_writer << c.info() << endl;
        if (!monitor->can_process(graphs[i]))
            cout << "FALSE";
        else
        {
            while (!monitor->is_terminal(c))
            {
                string oracle = monitor->get_oracle(c, graphs[i]);
                if (oracle == "-E-"){
                    error_cnt++;
                    cerr << endl<< "id:" << i  <<"len:" << sents[i].n << "first: " << sents[i].words[0] <<" ";
                    if (sents[i].n > 2)
                        cerr<<sents[i].words[1]<<" " << sents[i].words[2]<<endl;
                    cerr << c.info() << endl;
                    break;
                }
                monitor->apply(c, oracle);
                //cout << " " << oracle;
                oracle_writer << oracle << endl;
                oracle_writer << c.info() << endl;
            }
        }
        //cout << endl;
        //oracle_writer << endl;
    }
    cerr <<endl<< "#Error sentence number:" << error_cnt << endl;
}

void DependencyParser::extract_transition_sequence(string & train_file, string & oracle_file)
{
    extract_transition_sequence(train_file.c_str(), oracle_file.c_str());
}

void DependencyParser::gen_dictionaries_graph(
        vector<DependencySent> & sents,
        vector<DependencyGraph> & graphs)
{
    vector<string> all_words;
    vector<string> all_poss;
    vector<string> all_labels;
    vector<string> all_clusters;

    for (size_t i = 0; i < sents.size(); ++i)
    {
        /* debug
        sents[i].print_info();
        trees[i].print();
        */
        /*
        if (config.fix_word_embeddings)
        {
            for (size_t j = 0; j < sents[i].words.size(); ++j)
            {
                string word = sents[i].words[j];
                if (embed_ids.find(word) != embed_ids.end())
                    all_words.push_back(word);
            }
        }
        */
        all_words.insert(
                all_words.end(),
                sents[i].words.begin(),
                sents[i].words.end());

        all_poss.insert(
                all_poss.end(),
                sents[i].poss.begin(),
                sents[i].poss.end());

        if (config.use_cluster)
        {
            all_clusters.insert(
                    all_clusters.end(),
                    sents[i].clusters.begin(),
                    sents[i].clusters.end());
            /*
            vector<string> cluster_p4;
            get_prefix(sents[i].clusters, cluster_p4, 4);
            vector<string> cluster_p6;
            get_prefix(sents[i].clusters, cluster_p6, 6);

            all_clusters.insert(
                    all_clusters.end(),
                    cluster_p4.begin(),
                    cluster_p4.end());
            all_clusters.insert(
                    all_clusters.end(),
                    cluster_p6.begin(),
                    cluster_p6.end());
                    */
        }
    }

    string root_label = "";
    if (config.labeled)
    {
        for (size_t i = 0; i < graphs.size(); ++i)
        {
            for (int j = 1; j <= graphs[i].n; ++j)
            {
                vector<int> h = graphs[i].get_head(j);
                vector<std::string> l = graphs[i].get_label(j);
                for (int k = 0; k < (int)h.size(); k++){
                    if (h[k] == 0){
                        root_label = l[k];
                    } 
                    else{
                        string label = l[k];
                        if (label != root_label)
                            all_labels.push_back(label);
                    }
                }
            }
        }
    }

    known_words  = Util::generate_dict(all_words, config.word_cut_off);
    known_poss   = Util::generate_dict(all_poss);
    known_labels = Util::generate_dict(all_labels);

    if (config.use_cluster)
        known_clusters = Util::generate_dict(all_clusters);

    /**
     * Usage of these symbols:
     *
     *  UNKNOWN - Out-Of-Vocabulary words/poss
     *  NIL     - Non-Exist words/pos/label
     *  ROOT    - word form and pos for ROOT
     */
    known_words.push_back(Config::UNKNOWN);
    known_words.push_back(Config::NIL);
    known_words.push_back(Config::ROOT);

    known_poss.push_back(Config::UNKNOWN);
    known_poss.push_back(Config::NIL);
    known_poss.push_back(Config::ROOT);

    if (config.use_cluster)
    {
        // Config::UNKNOWN has already been in known_clusters
        known_clusters.push_back(Config::UNKNOWN);
        known_clusters.push_back(Config::NIL);
        known_clusters.push_back(Config::ROOT);
    }

    if (config.labeled)
    {
        /**
         * In case that there are multiple root_labels
         *  which are unnecessarily attached to w_0
         *
         * In fact, this is not kind of legal annotation,
         *  however, it indeed appears in the universal
         *  dependency treebanks.
         */
        if (find(known_labels.begin(),
                    known_labels.end(),
                    root_label)
                == known_labels.end())
            known_labels.push_back(root_label);
        known_labels.push_back(Config::NIL);
    }
    else
    {
        known_labels.push_back(Config::UNKNOWN);
    }

    /**
     * find all oracle decisions, and extract dynamic features
     *  - can add other features here, aside from /distance/
     */
    cerr << "collect dynamic features (e.g. distances)" << endl;
    if (config.use_distance || config.use_valency || config.use_length)
    {
        collect_dynamic_features(sents, graphs);
        /*
        known_distances.push_back(1);
        known_distances.push_back(2);
        known_distances.push_back(3);
        known_distances.push_back(4);
        known_distances.push_back(5);
        known_distances.push_back(6);
        */
        known_distances.push_back(Config::UNKNOWN_INT);
        known_lengths.push_back(Config::UNKNOWN_INT);
        // known_valencies.push_back(Config::UNKNOWN);
    }

    generate_ids();

    cerr << config.SEPERATOR << endl;
    cerr << "#Word:     " << known_words.size()    << endl;
    cerr << "#POS:      " << known_poss.size()     << endl;
    cerr << "#Clusters: " << known_clusters.size() << endl;

    if (config.labeled)
    {
        cerr << "#Label: " << known_labels.size() << endl;
        for (size_t i = 0; i < known_labels.size(); ++i)
            cerr << known_labels[i] << ", ";
        cerr << endl;
    }

    if (config.use_distance)
        cerr << "#Distances: " << known_distances.size() << endl;
    if (config.use_valency)
    {
        cerr << "#Valencies: " << known_valencies.size() << endl;
        for (size_t j = 0; j < known_valencies.size(); ++j)
            cerr << known_valencies[j] << ", ";
        cerr << endl;
    }
    if (config.use_length)
        cerr << "#Length: " << known_lengths.size() << endl;
}

void DependencyParser::collect_dynamic_features(
        vector<DependencySent> & sents,
        vector<DependencyGraph> & graphs)
{
    vector<int> all_distances;
    vector<string> all_valencies;
    vector<int> all_lengths;

    vector<string> ldict = known_labels;
    ldict.pop_back();
    ParsingSystem * monitor;
    if (config.oracle == "arceager")
        monitor =  new ArcEager(ldict, config.language, config.labeled);
    else if (config.oracle == "listsystem")
        monitor =  new ListSystem(ldict, config.language, config.labeled);

    for (size_t i = 0; i < sents.size(); ++i)
    {
        // if (i == 20652) continue;
        // runtime info
        // cerr << "\r" << i;
        // trees[i].print();
        // if (trees[i].is_projective())
        // if (trees[i].is_single_root()) // non-projective
        // if (trees[i].is_single_root() || trees[i].is_projective()) // all trees
        if (monitor->can_process(graphs[i]))
        {
            Configuration c(sents[i]);
            while (!monitor->is_terminal(c))
            {
                string oracle = monitor->get_oracle(c, graphs[i]);
                //cerr << "Oracle: " << oracle << endl;
                //cerr << "Configuration: " << c.info() << endl;
                monitor->apply(c, oracle);

                all_distances.push_back(c.get_distance());
                all_lengths.push_back(c.get_pass_buffer_size());

                //int k = c.get_stack(1);
                int k = c.get_stack(0);
                all_valencies.push_back(c.get_lvalency(k));
                all_valencies.push_back(c.get_rvalency(k));
                all_valencies.push_back(c.get_lhvalency(k));
                all_valencies.push_back(c.get_rhvalency(k));

                k = c.get_buffer(0);
                all_valencies.push_back(c.get_lvalency(k));
                all_valencies.push_back(c.get_rvalency(k));
                all_valencies.push_back(c.get_lhvalency(k));
                all_valencies.push_back(c.get_rhvalency(k));
            }
        }
    }

    monitor = NULL; delete monitor;

    known_distances = Util::generate_dict(all_distances);
    all_valencies.push_back(Config::UNKNOWN);
    known_valencies = Util::generate_dict(all_valencies);
    known_lengths = Util::generate_dict(all_lengths);
}

void DependencyParser::setup_classifier_for_training(
        vector<DependencySent> & sents,
        vector<DependencyGraph> & graphs,
        const char * embed_file,
        const char * premodel_file)
{
    int Eb_entries = 0;
    int Ed_entries = 0, Ev_entries = 0, Ec_entries = 0, El_entries = 0;

    if (config.use_postag)
        Eb_entries = known_poss.size();
    if (config.labeled)
        Eb_entries += known_labels.size();
    if (!config.delexicalized)
        Eb_entries += known_words.size();

    if (config.use_distance)
        Ed_entries = known_distances.size();
    if (config.use_valency)
        Ev_entries = known_valencies.size();
    if (config.use_cluster)
        Ec_entries = known_clusters.size();
    if (config.use_length)
        El_entries = known_lengths.size();

    Mat<double> Eb(0.0, Eb_entries, config.embedding_size);
    Mat<double> Ed(0.0, Ed_entries, config.distance_embedding_size);
    Mat<double> Ev(0.0, Ev_entries, config.valency_embedding_size);
    Mat<double> Ec(0.0, Ec_entries, config.cluster_embedding_size);
    Mat<double> El(0.0, El_entries, config.length_embedding_size);

    int W1_ncol = config.embedding_size * config.num_basic_tokens;
    if (config.use_distance)
        W1_ncol += config.distance_embedding_size * config.num_dist_tokens;
    if (config.use_valency)
        W1_ncol += config.valency_embedding_size * config.num_valency_tokens;
    if (config.use_cluster)
        W1_ncol += config.cluster_embedding_size * config.num_cluster_tokens;
    if (config.use_length)
        W1_ncol += config.length_embedding_size * config.num_length_tokens;

    cerr << "W1_ncol = " << W1_ncol << endl;
    Mat<double> W1(0.0, config.hidden_size, W1_ncol);
    Vec<double> b1(0.0, config.hidden_size);
    int n_actions = 0;
    if (config.oracle == "arceager")
        n_actions = (config.labeled) ? (known_labels.size() * 4 - 4) : 7;// attach system
    else if (config.oracle == "listsystem")
        n_actions = (config.labeled) ? (known_labels.size() * 3 - 3) : 5;// list system
    Mat<double> W2(0.0, n_actions, config.hidden_size);

    // Randomly initialize weight matrices / vectors
    double W1_init_range = sqrt(6.0 / (W1.nrows() + W1.ncols()));
    #pragma omp parallel for
    for (int i = 0; i < W1.nrows(); ++i)
        for (int j = 0; j < W1.ncols(); ++j)
            W1[i][j] = (Util::rand_double() * 2 - 1) * W1_init_range;

    #pragma omp parallel for
    for (int i = 0; i < b1.size(); ++i)
        b1[i] = (Util::rand_double() * 2 - 1) * W1_init_range;

    double W2_init_range = sqrt(6.0 / (W2.nrows() + W2.ncols()));
    #pragma omp parallel for
    for (int i = 0; i < W2.nrows(); ++i)
        for (int j = 0; j < W2.ncols(); ++j)
            W2[i][j] = (Util::rand_double() * 2 - 1) * W2_init_range;

    // Match the embedding vocabulary with words in dictionary
    int in_embed = 0;
    #pragma omp parallel for
    for (int i = 0; i < Eb.nrows(); ++i)
    {
        if (config.delexicalized)
        {
            for (int j = 0; j < Eb.ncols(); ++j)
                Eb[i][j] = (Util::rand_double() * 2 - 1) * config.init_range;
            continue;
        }

        int index = -1;
        if (i < (int)known_words.size())
        {
            string word = known_words[i];
            if (embed_ids.find(word) != embed_ids.end())
                index = embed_ids[word];
            else if (embed_ids.find(str_tolower(word)) != embed_ids.end())
                index = embed_ids[str_tolower(word)];
            /**
             * if fix embeddings, then those words are all treated as UNK
             */
            /*
            else if (config.fix_embeddings
                        && word != Config::NIL
                        && word != Config::ROOT)
                index = embed_ids[Config::UNKNOWN];
            */
        }

        if (index >= 0)
        {
            ++ in_embed;
            for (int j = 0; j < Eb.ncols(); ++j)
                Eb[i][j] = embeddings[index][j];
        }
        else
        {
            for (int j = 0; j < Eb.ncols(); ++j)
                Eb[i][j] = (Util::rand_double() * 2 - 1) * config.init_range;
        }
    }
    #pragma omp parallel for
    for (int i = 0; i < Ed.nrows(); ++i)
        for (int j = 0; j < Ed.ncols(); ++j)
            Ed[i][j] = (Util::rand_double() * 2 - 1) * config.init_range;
    #pragma omp parallel for
    for (int i = 0; i < Ev.nrows(); ++i)
        for (int j = 0; j < Ev.ncols(); ++j)
            Ev[i][j] = (Util::rand_double() * 2 - 1) * config.init_range;
    #pragma omp parallel for
    for (int i = 0; i < Ec.nrows(); ++i)
        for (int j = 0; j < Ec.ncols(); ++j)
            Ec[i][j] = (Util::rand_double() * 2 - 1) * config.init_range;
    #pragma omp parallel for
    for (int i = 0; i < El.nrows(); ++i)
        for (int j = 0; j < El.ncols(); ++j)
            El[i][j] = (Util::rand_double() * 2 - 1) * config.init_range;

    cerr << "Found embeddings: "
         << in_embed
         << " / "
         << known_words.size()
         << endl;

    if (premodel_file[0] != 0)
    {
        // load model
        // load_model(premodel_file);
        cerr << "Load pre-trained model: " << premodel_file << endl;
        ifstream input(premodel_file);
        string s;
        getline(input, s); int n_dict = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int n_pos = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int n_label = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int n_dist = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int n_valency = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int n_cluster = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int n_length = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int Eb_size = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int Ed_size = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int Ev_size = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int Ec_size = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int El_size = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int h_size = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int n_basic_tokens = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int n_dist_tokens = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int n_valency_tokens = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int n_cluster_tokens = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int n_length_tokens = to_int(split_by_sep(s, "=")[1]);
        getline(input, s); int n_pre_computed = to_int(split_by_sep(s, "=")[1]);

        assert (h_size == config.hidden_size);
        assert (n_basic_tokens == config.num_basic_tokens);
        assert (n_dist_tokens == config.num_dist_tokens);
        assert (n_valency_tokens == config.num_valency_tokens);
        assert (n_cluster_tokens == config.num_cluster_tokens);
        assert (n_length_tokens == config.num_length_tokens);

        vector<string> sep;

        if (!config.delexicalized)
            for (int i = 0; i < n_dict; ++i)
            {
                getline(input, s);
                sep = split(s);
                int index = get_word_id(sep[0]);
                if (index != Config::NONEXIST)
                    for (int j = 0; j < Eb_size; ++j)
                        Eb[index][j] = to_double_sci(sep[j]);
            }
        if (config.use_postag)
            for (int i = 0; i < n_pos; ++i)
            {
                getline(input, s);
                sep = split(s);
                int index = get_pos_id(sep[0]);
                for (int j = 0; j < Eb_size; ++j)
                    Eb[index][j] = to_double_sci(sep[j]);
            }
        if (config.labeled)
            for (int i = 0; i < n_label; ++i)
            {
                getline(input, s);
                sep = split(s);
                int index = get_label_id(sep[0]);
                for (int j = 0; j < Eb_size; ++j)
                    Eb[index][j] = to_double_sci(sep[j+1]);
            }
        if (config.use_distance)
            for (int i = 0; i < n_dist; ++i)
            {
                getline(input, s);
                sep = split(s);
                int index = get_distance_id(to_int(sep[0]));
                for (int j = 0; j < Ed_size; ++j)
                    Ed[index][j] = to_double_sci(sep[j+1]);
            }
        if (config.use_valency)
            for (int i = 0; i < n_valency; ++i)
            {
                getline(input, s);
                sep = split(s);
                int index = get_valency_id(sep[0]);
                for (int j = 0; j < Ev_size; ++j)
                    Ev[index][j] = to_double_sci(sep[j+1]);
            }
        if (config.use_cluster)
            for (int i = 0; i < n_cluster; ++i)
            {
                getline(input, s);
                sep = split(s);
                int index = get_cluster_id(sep[0]);
                for (int j = 0; j < Ec_size; ++j)
                    Ec[index][j] = to_double_sci(sep[j+1]);
            }
        if (config.use_length)
            for (int i = 0; i < n_length; ++i)
            {
                getline(input, s);
                sep = split(s);
                int index = get_length_id(to_int(sep[0]));
                for (int j = 0; j < El_size; ++j)
                    El[index][j] = to_double_sci(sep[j+1]);
            }

        // set W1
        for (int j = 0; j < W1.ncols(); ++j)
        {
            getline(input, s);
            sep = split(s);
            for (int i = 0; i < W1.nrows(); ++i)
                W1[i][j] = to_double_sci(sep[i]);
        }

        // set b1
        getline(input, s);
        sep = split(s);
        for (int i = 0; i < b1.size(); ++i)
            b1[i] = to_double_sci(sep[i]);

        for (int j = 0; j < W2.ncols(); ++j)
        {
            getline(input, s);
            sep = split(s);
            for (int i = 0; i < W2.nrows(); ++i)
                W2[i][j] = to_double_sci(sep[i]);
        }

        input.close();
    }

    /**
     * generate training dataset and
     * determine the pre_computed ids (important)
     */
    Dataset dataset = gen_train_samples_graph(sents, graphs);

    // shuffle dataset
    // cerr << "shuffle training set" << endl;
    // random_shuffle(dataset.samples.begin(), dataset.samples.end());

    /**
     * shuffle the dataset
     *  - little help
     */
    // cerr << "Shuffling training samples" << endl;
    // dataset.shuffle();

    /**
     * setup the classifier
     */
    cerr << "create classifier" << endl;
    classifier = new NNClassifier(config, dataset, Eb, Ed, Ev, Ec, El, W1, b1, W2, pre_computed_ids);
}

void DependencyParser::generate_ids()
{
    int index = 0;

    for (size_t i = 0; i < known_words.size(); ++i)
        word_ids[known_words[i]] = index++;

    if (config.delexicalized) // not counting words
        index = 0;

    for (size_t i = 0; i < known_poss.size();  ++i)
    {
        if (!config.use_postag)
            pos_ids[known_poss[i]] = 0;
        else
            pos_ids[known_poss[i]] = index++;
    }

    // if (config.labeled)
    for (size_t i = 0; i < known_labels.size(); ++i)
        label_ids[known_labels[i]] = index++;

    if (config.use_distance)
        for (size_t i = 0; i < known_distances.size(); ++i)
            distance_ids[known_distances[i]] = index++;

    if (config.use_valency)
        for (size_t i = 0; i < known_valencies.size(); ++i)
            valency_ids[known_valencies[i]] = index++;

    if (config.use_cluster)
        for (size_t i = 0; i < known_clusters.size(); ++i)
            cluster_ids[known_clusters[i]] = index++;

    if (config.use_length)
        for (size_t i = 0; i < known_lengths.size(); ++i)
            length_ids[known_lengths[i]] = index++;
    /* debug
    cerr << "word_ids" << endl;
    for (map<string, int>::iterator iter = word_ids.begin();
            iter != word_ids.end(); ++iter)
        cerr << iter->first << ": " << iter->second << ", ";
    cerr << endl;

    cerr << "pos_ids" << endl;
    for (map<string, int>::iterator iter = pos_ids.begin();
            iter != pos_ids.end(); ++iter)
        cerr << iter->first << ": " << iter->second << ", ";
    cerr << endl;

    cerr << "label_ids" << endl;
    for (map<string, int>::iterator iter = label_ids.begin();
            iter != label_ids.end(); ++iter)
        cerr << iter->first << ": " << iter->second << ", ";
    cerr << endl;
    */
}

Dataset DependencyParser::gen_train_samples_graph(
        vector<DependencySent> & sents,
        vector<DependencyGraph> & graphs)
{
    int num_trans = system->transitions.size();
    Dataset ds_train(config.num_tokens, num_trans);

    cerr << Config::SEPERATOR << endl;
    cerr << "Generating training examples..." << endl;
    unordered_map<int, int> tokpos_count;

    int error_cnt = 0;
    for (size_t i = 0; i < sents.size(); ++i)
    {
        // runtime info
        if (i > 0)
        {
            if (i % 1000 == 0)
                cerr << i << " ";
            if (i % 10000 == 0 || i == sents.size() - 1)
                cerr << endl;
        }

        /**
         * only use the projective trees
         *
         * TODO: non-projective trees also contain useful
         *  information for transition. try exploiting them.
         */
        // if (trees[i].is_projective())
        // if (trees[i].is_single_root()) // non-projective
        // if (trees[i].is_single_root() || trees[i].is_projective()) // all trees
         
       /* cerr << endl<< "id:" << i << "first:" << sents[i].words[0] <<" ";
        if (sents[i].n > 2)
            cerr<<sents[i].words[1]<<" " << sents[i].words[2]<<endl;*/
        if (system->can_process(graphs[i]))
        {
            // cerr << i << " is projective" << endl;
            Configuration c(sents[i]);
            while (!system->is_terminal(c))
            {
                string oracle = system->get_oracle(c, graphs[i]);
                //cerr << oracle <<" ";
                if (oracle == "-E-"){
                    error_cnt++;
                    cerr << endl<< "id:" << i  <<"len:" << sents[i].n << "first: " << sents[i].words[0] <<" ";
                    if (sents[i].n > 2)
                        cerr<<sents[i].words[1]<<" " << sents[i].words[2]<<endl;
                    cerr << c.info() << endl;
                    break;
                }
                vector<int> features = get_features(c);
                // int label = system->get_transition_id(oracle);
                vector<int> label(num_trans, -1);
                for (int j = 0; j < num_trans; ++j)
                {
                    string action = system->transitions[j];
                    if (action == oracle) label[j] = 1;
                    else if (system->can_apply(c, action)) label[j] = 0;
                    // else label.push_back(-1);
                }

                ds_train.add_sample(features, label);
                for (size_t j = 0; j < features.size(); ++j)
                {
                    int feature_id = features[j] * features.size() + j;
                    if (tokpos_count.find(feature_id) == tokpos_count.end())
                        tokpos_count[feature_id] = 1;
                    else
                        tokpos_count[feature_id] += 1;
                }
                system->apply(c, oracle);
            }
        }
    }
    cerr <<endl<< "#Error sentence number:" << error_cnt << endl;

    cerr << "#Train examples: " << ds_train.n << endl;

    /**
     * Determine the pre-computed feature IDs.
     *
     * NB: both {tok, pos} are taken into acocunt
     *  in order to attain the feature ID.
     */
    // /* debug
    vector<pair<int, int>> temp(tokpos_count.begin(), tokpos_count.end());
    cerr << "sort tokpos_count" << endl;
    sort(temp.begin(), temp.end(), Util::comp_by_value_descending<int, int>);

    pre_computed_ids.clear();
    cerr << "fill pre_computed_ids" << endl;
    int real_size = min((int)temp.size(), config.num_pre_computed);
    for (int i = 0; i < real_size; ++i)
        pre_computed_ids.push_back(temp[i].first);
    // */

    return ds_train;
}

void DependencyParser::scan_test_samples(
        vector<DependencySent> & sents,
        vector<DependencyGraph> & graphs,
        vector<int> & precompute_ids)
{
    unordered_map<int, int> tokpos_count;
    precompute_ids.clear();

    for (size_t i = 0; i < sents.size(); ++i)
    {
        // if (trees[i].is_projective())
        // if (trees[i].is_single_root()) // non-projective
        // if (trees[i].is_single_root() || trees[i].is_projective()) // all trees
        int error_cnt = 0;
        if (system->can_process(graphs[i]))
        {
            Configuration c(sents[i]);
            while (!system->is_terminal(c))
            {
                string oracle = system->get_oracle(c, graphs[i]);
                //cerr << "oracle:" << oracle << " ";
                if (oracle == "-E-"){
                    error_cnt++;
                    cerr << endl<< "id:" << i  <<"len:" << sents[i].n << "first: " << sents[i].words[0] <<" ";
                    if (sents[i].n > 2)
                        cerr<<sents[i].words[1]<<" " << sents[i].words[2]<<endl;
                    cerr << c.info() << endl;
                    break;
                }
                vector<int> features = get_features(c);

                for (size_t j = 0; j < features.size(); ++j)
                {
                    int feature_id = features[j] * features.size() + j;
                    if (tokpos_count.find(feature_id) == tokpos_count.end())
                        tokpos_count[feature_id] = 1;
                    else
                        tokpos_count[feature_id] += 1;
                }

                system->apply(c, oracle);
            }
        }
    }

    vector<pair<int, int>> temp(tokpos_count.begin(), tokpos_count.end());
    cerr << "sort tokpos_count" << endl;
    sort(temp.begin(), temp.end(), Util::comp_by_value_descending<int, int>);

    cerr << "fill pre_computed_ids" << endl;
    int real_size = min((int)temp.size(), config.num_pre_computed);
    for (int i = 0; i < real_size; ++i)
        precompute_ids.push_back(temp[i].first);
}

void DependencyParser::read_embed_file(const char * embed_file)
{
    ifstream emb_reader(embed_file);

    // embeddings.resize(0, 0);
    embed_ids.clear();

    if (emb_reader.fail())
    {
        cerr << "# fail to open embedding file ("
             << embed_file << "), "
             << "randomly initialize."
             << endl;
        return ;
    }

    vector<string> lines;
    string line;
    while (getline(emb_reader, line))
    {
        // getline(emb_reader, line);
        // emb_reader >> line;
        if (line.length() != 0)
            lines.push_back(line);
    }

    int nwords = lines.size();
    int dim = split(lines[0]).size() - 1;

    cerr << "Embedding file: " << embed_file << endl
         << "#Words = " <<  nwords << endl
         << "#Dim   = " << dim
         << endl;

    if (dim != config.embedding_size)
        cerr << "ERROR: embedding dimension mismatch"
             << endl;

    embeddings.resize(nwords, dim);
    for (int i = 0; i < nwords; ++i)
        for (int j = 0; j < dim; ++j)
            embeddings[i][j] = 0;

    for (int i = 0; i < nwords; ++i)
    {
        vector<string> sep = split(lines[i]);

        embed_ids[sep[0]] = i;
        for (int j = 0; j < dim; ++j)
            embeddings[i][j] = to_double_sci(sep[j + 1]);
    }
}

void DependencyParser::save_model(const string & filename)
{
    save_model(filename.c_str());
}

void DependencyParser::save_model(const char * filename)
{
    /**
     * write model file along with pre-computed matrix
     * into the specified file.
     */
    Mat<double>& W1 = classifier->get_W1();
    Mat<double>& W2 = classifier->get_W2();
    Vec<double>& b1 = classifier->get_b1();
    Mat<double>& Eb = classifier->get_Eb();
    Mat<double>& Ed = classifier->get_Ed();
    Mat<double>& Ev = classifier->get_Ev();
    Mat<double>& Ec = classifier->get_Ec();
    Mat<double>& El = classifier->get_El();

    ofstream output(filename);
    output << "dict=" << known_words.size() << "\n"
           << "pos=" << known_poss.size()  << "\n"
           << "label=" << known_labels.size() << "\n"
           << "dist=" << known_distances.size() << "\n"
           << "valency=" << known_valencies.size() << "\n"
           << "cluster=" << known_clusters.size() << "\n"
           << "length=" << known_lengths.size() << "\n"
           << "embeddingsize=" << Eb.ncols() << "\n"
           << "distembeddingsize=" << Ed.ncols() << "\n"
           << "valencyembeddingsize=" << Ev.ncols() << "\n"
           << "clusterembeddingsize=" << Ec.ncols() << "\n"
           << "lengthembeddingsize=" << El.ncols() << "\n"
           << "hiddensize=" << b1.size() << "\n"
           << "basictokens=" << config.num_basic_tokens << "\n"
           << "disttokens="  << config.num_dist_tokens << "\n"
           << "valencytokens=" << config.num_valency_tokens << "\n"
           << "clustertokens=" << config.num_cluster_tokens << "\n"
           << "lengthtokens=" << config.num_length_tokens << "\n"
           << "precomputed=" << pre_computed_ids.size() << "\n";

    int index = 0;
    // write word/pos/label embeddings
    if (!config.delexicalized)
        for (size_t i = 0; i < known_words.size(); ++i)
        {
            output << known_words[i];
            for (int j = 0; j < Eb.ncols(); ++j)
                output << " " << Eb[index][j];
            output << "\n";
            index += 1;
        }
    if (config.use_postag)
        for (size_t i = 0; i < known_poss.size(); ++i)
        {
            output << known_poss[i];
            for (int j = 0; j < Eb.ncols(); ++j)
                output << " " << Eb[index][j];
            output << "\n";
            index += 1;
        }
    if (config.labeled)
        for (size_t i = 0; i < known_labels.size(); ++i)
        {
            output << known_labels[i];
            for (int j = 0; j < Eb.ncols(); ++j)
                output << " " << Eb[index][j];
            output << "\n";
            index += 1;
        }
    index = 0;
    if (config.use_distance)
        for (size_t i = 0; i < known_distances.size(); ++i)
        {
            output << known_distances[i];
            for (int j = 0; j < Ed.ncols(); ++j)
                output << " " << Ed[index][j];
            output << "\n";
            index += 1;
        }
    index = 0;
    if (config.use_valency)
        for (size_t i = 0; i < known_valencies.size(); ++i)
        {
            output << known_valencies[i];
            for (int j = 0; j < Ev.ncols(); ++j)
                output << " " << Ev[index][j];
            output << "\n";
            index += 1;
        }
    index = 0;
    if (config.use_cluster)
        for (size_t i = 0; i < known_clusters.size(); ++i)
        {
            output << known_clusters[i];
            for (int j = 0; j < Ec.ncols(); ++j)
                output << " " << Ec[index][j];
            output << "\n";
            index += 1;
        }
    index = 0;
    if (config.use_length)
        for (size_t i = 0; i < known_lengths.size(); ++i)
        {
            output << known_lengths[i];
            for (int j = 0; j < El.ncols(); ++j)
                output << " " << El[index][j];
            output << "\n";
            index += 1;
        }

    // write classifier weights
    for (int j = 0; j < W1.ncols(); ++j)
    {
        output << W1[0][j];
        for (int i = 1; i < W1.nrows(); ++i)
            output << " " << W1[i][j];
        output << "\n";
    }
    output << b1[0];
    for (int i = 1; i < b1.size(); ++i)
        output << " " << b1[i];
    output << "\n";
    for (int j = 0; j < W2.ncols(); ++j)
    {
        output << W2[0][j];
        for (int i = 1; i < W2.nrows(); ++i)
            output << " " << W2[i][j];
        output << "\n";
    }

    // write pre-computed info (feature ids)
    for (size_t i = 0; i < pre_computed_ids.size(); ++i)
    {
        output << pre_computed_ids[i];
        if ((i + 1) % 100 == 0 ||
                i == pre_computed_ids.size() - 1)
            output << "\n";
        else
            output << " ";
    }

    output.close();
}

vector<int> DependencyParser::get_features(Configuration& c)
{
    vector<int> f_word;
    vector<int> f_pos;
    vector<int> f_label;
    vector<int> f_cluster;

    for (int i = 1; i >= 0; --i) // S0-S4:w,p
    {
        int index = c.get_stack(i);
        f_word.push_back(get_word_id(c.get_word(index)));
        f_pos.push_back(get_pos_id(c.get_pos(index)));
        f_cluster.push_back(get_cluster_id(c.get_cluster(index)));
    }
    for (int i = 0; i <= 1; ++i) // N0,N1:w,p
    {
        int index = c.get_buffer(i);
        f_word.push_back(get_word_id(c.get_word(index)));
        f_pos.push_back(get_pos_id(c.get_pos(index)));
        f_cluster.push_back(get_cluster_id(c.get_cluster(index)));
    }

    int index = c.get_pass_buffer(0); // pass buffer 0:w,p,c
    f_word.push_back(get_word_id(c.get_word(index)));
    f_pos.push_back(get_pos_id(c.get_pos(index)));
    f_cluster.push_back(get_cluster_id(c.get_cluster(index)));

    int k = c.get_stack(0);
    index = c.get_left_child(k); // S0l:wpl
    f_word.push_back(get_word_id(c.get_word(index)));
    f_pos.push_back(get_pos_id(c.get_pos(index)));
    f_label.push_back(get_label_id(c.get_arc_label(index, k)));
    f_cluster.push_back(get_cluster_id(c.get_cluster(index)));

    index = c.get_right_child(k); //S0r:wpl
    f_word.push_back(get_word_id(c.get_word(index)));
    f_pos.push_back(get_pos_id(c.get_pos(index)));
    f_label.push_back(get_label_id(c.get_arc_label(index, k)));
    f_cluster.push_back(get_cluster_id(c.get_cluster(index)));

    index = c.get_left_child(c.get_left_child(k)); //S0ll:wpl
    f_word.push_back(get_word_id(c.get_word(index)));
    f_pos.push_back(get_pos_id(c.get_pos(index)));
    f_label.push_back(get_label_id(c.get_arc_label(index, c.get_left_child(k))));
    f_cluster.push_back(get_cluster_id(c.get_cluster(index)));

    index = c.get_right_child(c.get_right_child(k)); //S0rr:wpl
    f_word.push_back(get_word_id(c.get_word(index)));
    f_pos.push_back(get_pos_id(c.get_pos(index)));
    f_label.push_back(get_label_id(c.get_arc_label(index, c.get_right_child(k))));
    f_cluster.push_back(get_cluster_id(c.get_cluster(index)));

    index = c.get_left_head(k); //S0lh:wpl
    f_word.push_back(get_word_id(c.get_word(index)));
    f_pos.push_back(get_pos_id(c.get_pos(index)));
    f_label.push_back(get_label_id(c.get_arc_label(index, k)));
    f_cluster.push_back(get_cluster_id(c.get_cluster(index)));

    index = c.get_right_head(k); //S0rh:wpl
    f_word.push_back(get_word_id(c.get_word(index)));
    f_pos.push_back(get_pos_id(c.get_pos(index)));
    f_label.push_back(get_label_id(c.get_arc_label(index, k)));
    f_cluster.push_back(get_cluster_id(c.get_cluster(index)));

    index = c.get_left_head(c.get_left_head(k)); //S0llh:wpl
    f_word.push_back(get_word_id(c.get_word(index)));
    f_pos.push_back(get_pos_id(c.get_pos(index)));
    f_label.push_back(get_label_id(c.get_arc_label(index, c.get_left_child(k))));
    f_cluster.push_back(get_cluster_id(c.get_cluster(index)));

    index = c.get_right_head(c.get_right_head(k)); //S0rrh:wpl
    f_word.push_back(get_word_id(c.get_word(index)));
    f_pos.push_back(get_pos_id(c.get_pos(index)));
    f_label.push_back(get_label_id(c.get_arc_label(index, c.get_right_child(k))));
    f_cluster.push_back(get_cluster_id(c.get_cluster(index)));

    k = c.get_buffer(0);
    index = c.get_left_child(k); //N0lc:wpl
    f_word.push_back(get_word_id(c.get_word(index)));
    f_pos.push_back(get_pos_id(c.get_pos(index)));
    f_label.push_back(get_label_id(c.get_arc_label(index, k)));
    f_cluster.push_back(get_cluster_id(c.get_cluster(index)));

    index = c.get_left_head(k); //N0lh:wpl
    f_word.push_back(get_word_id(c.get_word(index)));
    f_pos.push_back(get_pos_id(c.get_pos(index)));
    f_label.push_back(get_label_id(c.get_arc_label(index, k)));
    f_cluster.push_back(get_cluster_id(c.get_cluster(index)));

    index = c.get_left_child(c.get_left_child(k)); //N0llc:wpl
    f_word.push_back(get_word_id(c.get_word(index)));
    f_pos.push_back(get_pos_id(c.get_pos(index)));
    f_label.push_back(get_label_id(c.get_arc_label(index, c.get_left_child(k))));
    f_cluster.push_back(get_cluster_id(c.get_cluster(index)));

    index = c.get_left_head(c.get_left_head(k)); //N0llh:wpl
    f_word.push_back(get_word_id(c.get_word(index)));
    f_pos.push_back(get_pos_id(c.get_pos(index)));
    f_label.push_back(get_label_id(c.get_arc_label(index, c.get_right_child(k))));
    f_cluster.push_back(get_cluster_id(c.get_cluster(index)));

    vector<int> features;
    if (!config.delexicalized)
        features.insert(features.end(),
                        f_word.begin(),
                        f_word.end());

    if (config.use_postag)
        features.insert(features.end(),
                        f_pos.begin(),
                        f_pos.end());

    if (config.labeled)
        features.insert(features.end(),
                        f_label.begin(),
                        f_label.end());

    if (config.use_distance)
        features.push_back(get_distance_id(c.get_distance()));
    
    if (config.use_valency)
    {
        int index = c.get_stack(0);
        features.push_back(get_valency_id(c.get_lvalency(index)));
        features.push_back(get_valency_id(c.get_rvalency(index)));
        features.push_back(get_valency_id(c.get_lhvalency(index)));
        features.push_back(get_valency_id(c.get_rhvalency(index)));
        index = c.get_buffer(0);
        features.push_back(get_valency_id(c.get_lvalency(index)));
        features.push_back(get_valency_id(c.get_lhvalency(index)));
    }
    if (config.use_cluster)
    {
        features.insert(features.end(),
                        f_cluster.begin(),
                        f_cluster.end());
    }

    if (config.use_length)
        features.push_back(get_length_id(c.get_pass_buffer_size()));

    assert ((int)features.size() == config.num_tokens);

    return features;
}

int DependencyParser::get_word_id(const string & s)
{
    // if fix_word_embeddings, then ignore cases
    string sl = s; // lower case form of s (if necessary)
    // when is it necessary to convert_to_lower_case
    // if (config.fix_word_embeddings)
    //     sl = str_tolower(sl);

    // adapt to `delexicalized`, introduce Config::NONEXIST
    /*
    return (word_ids.find(sl) == word_ids.end())
                ? ((word_ids.find(Config::UNKNOWN) == word_ids.end())
                        ? Config::NONEXIST
                        : word_ids[Config::UNKNOWN])
                : word_ids[sl];
    */
    if (word_ids.find(sl) == word_ids.end())
    {
        sl = str_tolower(sl);
        if (word_ids.find(sl) != word_ids.end())
            return word_ids[sl];
        else
        {
            if (word_ids.find(Config::UNKNOWN) == word_ids.end())
                return Config::NONEXIST;
            else
                return word_ids[Config::UNKNOWN];
        }
    }
    else
        return word_ids[sl];
}

int DependencyParser::get_pos_id(const string & s)
{
    return (pos_ids.find(s) == pos_ids.end())
                ? pos_ids[Config::UNKNOWN]
                : pos_ids[s];
}

int DependencyParser::get_label_id(const string & s)
{
    return label_ids[s];
}

int DependencyParser::get_distance_id(const int & d)
{
    return (distance_ids.find(d) == distance_ids.end())
                ? distance_ids[Config::UNKNOWN_INT]
                : distance_ids[d];
}

int DependencyParser::get_length_id(const int & d)
{
    return (length_ids.find(d) == length_ids.end())
                ? length_ids[Config::UNKNOWN_INT]
                : length_ids[d];
}

int DependencyParser::get_valency_id(const string & v)
{
    return (valency_ids.find(v) == valency_ids.end())
                ? valency_ids[Config::UNKNOWN]
                : valency_ids[v];
}

int DependencyParser::get_cluster_id(const string & c)
{
    return (cluster_ids.find(c) == cluster_ids.end())
                ? cluster_ids[Config::UNKNOWN]
                : cluster_ids[c];
}

void DependencyParser::predict_graph(
        DependencySent& sent,
        DependencyGraph& graph)
{
    int num_trans = system->transitions.size();
    Configuration c(sent);
    while (!system->is_terminal(c))
    {
        vector<double> scores;
        vector<int> features = get_features(c);
        classifier->compute_scores(features, scores);
        double opt_score = -DBL_MAX;
        string opt_trans = "";

        for (int i = 0; i < num_trans; ++i)
        {
            if (scores[i] > opt_score)
            {
                if (system->can_apply(c, system->transitions[i]))
                {
                    opt_score = scores[i];
                    opt_trans = system->transitions[i];
                }
                /*
                else
                {
                    cerr << system->transitions[i] << " can not apply" << endl;
                    cerr << "#configuration:" << endl
                         << "stack = " << c.stack.size() << ", " << c.get_stack(0) << " | " << c.get_stack(1) << endl
                         << "buffer = " << c.buffer.size() << ", " << c.get_buffer(0) << endl;
                }
                */
            }
        }
        if (opt_trans == "NS"){
            double snd_score = -DBL_MAX;
            string snd_trans = "";
            for (int i = 0; i < num_trans; ++i)
                if (scores[i] > snd_score)
                {
                    if (system->can_apply(c, system->transitions[i])){
                       // cerr <<"can apply:"<< system->transitions[i]<<endl;
                        if ((startswith(system->transitions[i],"L") || startswith(system->transitions[i],"R"))){
                        snd_trans = system->transitions[i];
                        snd_score = scores[i];}
                    }
                }
            c.save_2nd_head(snd_trans, snd_score);
            //cerr << "second trans:"<< snd_trans << " ";
        }
        //cerr << opt_trans << "->" << endl;
        //cerr << "true: " << opt_trans << endl;
        system->apply(c, opt_trans);

        //cerr <<endl<< c.info() << endl;
        
        //cerr << c.info() << endl;
    }
    //c.graph.print();
    if (c.get_stack_size() > 1){
        process_headless(c);
    }
    if (!c.is_graph()){
        cerr << "error:not a graph!"<<endl;
        c.graph.print();
    }
    graph = c.graph;
    // return c.tree;
}

void DependencyParser::predict_graph(
        vector<DependencySent>& sents,
        vector<DependencyGraph>& graphs)
{
    // vector<DependencyTree> result;
    graphs.clear();
    graphs.resize(sents.size());
    // #pragma omp parallel for
    for (size_t i = 0; i < sents.size(); ++i)
    {
        cerr << "\r" << i << "    ";
        predict_graph(sents[i], graphs[i]);
        // result.push_back(predict(sents[i]));
    }
    cerr << endl;
    // return result;
}

void DependencyParser::load_model(const char * filename, bool re_precompute)
{
    cerr << "Loading depparse model from " << filename << endl;

    double start = get_time();

    ifstream input(filename);

    string s;
    getline(input, s); int n_dict = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_pos = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_label = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_dist = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_valency = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_cluster = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_length = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int Eb_size = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int Ed_size = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int Ev_size = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int Ec_size = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int El_size = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int h_size = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_basic_tokens = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_dist_tokens = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_valency_tokens = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_cluster_tokens = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_length_tokens = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_pre_computed = to_int(split_by_sep(s, "=")[1]);

    known_words.clear();
    known_poss.clear();
    known_labels.clear();
    known_distances.clear();
    known_valencies.clear();
    known_clusters.clear();
    known_lengths.clear();

    int index = 0;

    /*
    if (config.use_postag)      Eb_entries += n_pos;
    if (config.labeled)         Eb_entries += n_label;
    if (!config.delexicalized)  Eb_entries += n_dict;
    if (config.use_distance)    Ed_entries = n_dist;
    if (config.use_valency)     Ev_entries = n_valency;
    if (config.use_cluster)     Ec_entries = n_cluster;
    */

    int Eb_entries = n_label;
    if (!config.delexicalized) Eb_entries += n_dict;
    if (config.use_postag)     Eb_entries += n_pos;

    int Ed_entries = n_dist;
    int Ev_entries = n_valency;
    int Ec_entries = n_cluster;
    int El_entries = n_length;

    Mat<double> Eb(Eb_entries, Eb_size);
    Mat<double> Ed(Ed_entries, Ed_size);
    Mat<double> Ev(Ev_entries, Ev_size);
    Mat<double> Ec(Ec_entries, Ec_size);
    Mat<double> El(El_entries, El_size);

    if (!config.delexicalized)
        for (int i = 0; i < n_dict; ++i)
        {
            getline(input, s);
            vector<string> sep = split(s);
            known_words.push_back(sep[0]);

            assert (sep.size() == Eb_size + 1);
            for (int j = 0; j < Eb_size; ++j)
                Eb[index][j] = to_double_sci(sep[j+1]);
            index += 1;
        }

    if (config.use_postag)
        for (int i = 0; i < n_pos; ++i)
        {
            getline(input, s);
            vector<string> sep = split(s);
            known_poss.push_back(sep[0]);

            assert (sep.size() == Eb_size + 1);
            for (int j = 0; j < Eb_size; ++j)
                Eb[index][j] = to_double_sci(sep[j+1]);
            index += 1;
        }

    if (config.labeled)
        for (int i = 0; i < n_label; ++i)
        {
            getline(input, s);
            vector<string> sep = split(s);
            known_labels.push_back(sep[0]);

            assert (sep.size() == Eb_size + 1);
            for (int j = 0; j < Eb_size; ++j)
                Eb[index][j] = to_double_sci(sep[j+1]);
            index += 1;
        }
    else
    {
        known_labels.push_back(Config::UNKNOWN); // confused =.=
    }

    index = 0; // reset
    if (config.use_distance)
        for (int i = 0; i < n_dist; ++i)
        {
            getline(input, s);
            vector<string> sep = split(s);
            known_distances.push_back(to_int(sep[0]));

            assert (sep.size() == Ed_size + 1);
            for (int j = 0; j < Ed_size; ++j)
                Ed[index][j] = to_double_sci(sep[j+1]);
            index += 1;
        }

    index = 0; // reset
    if (config.use_valency)
        for (int i = 0; i < n_valency; ++i)
        {
            getline(input, s);
            vector<string> sep = split(s);
            known_valencies.push_back(sep[0]);

            assert (sep.size() == Ev_size + 1);
            for (int j = 0; j < Ev_size; ++j)
                Ev[index][j] = to_double_sci(sep[j+1]);
            index += 1;
        }

    index = 0; // reset
    if (config.use_cluster)
        for (int i = 0; i < n_cluster; ++i)
        {
            getline(input, s);
            vector<string> sep = split(s);
            known_clusters.push_back(sep[0]);

            assert (sep.size() == Ec_size + 1);
            for (int j = 0; j < Ec_size; ++j)
                Ec[index][j] = to_double_sci(sep[j+1]);
            index += 1;
        }

    index = 0; // reset
    if (config.use_length)
        for (int i = 0; i < n_length; ++i)
        {
            getline(input, s);
            vector<string> sep = split(s);
            known_lengths.push_back(to_int(sep[0]));

            assert (sep.size() == El_size + 1);
            for (int j = 0; j < El_size; ++j)
                El[index][j] = to_double_sci(sep[j+1]);
            index += 1;
        }

    generate_ids();

    int W1_ncol = Eb_size * n_basic_tokens
                + Ed_size * n_dist_tokens
                + Ev_size * n_valency_tokens
                + Ec_size * n_cluster_tokens
                + El_size * n_length_tokens;

    Mat<double> W1(h_size, W1_ncol);
    for (int j = 0; j < W1.ncols(); ++j)
    {
        getline(input, s);
        vector<string> sep = split(s);

        assert (sep.size() == h_size);
        for (int i = 0; i < W1.nrows(); ++i)
            W1[i][j] = to_double_sci(sep[i]);
    }

    Vec<double> b1(h_size);
    getline(input, s);
    vector<string> sep = split(s);
    assert (sep.size() == h_size);
    for (int i = 0; i < b1.size(); ++i)
    {
        b1[i] = to_double_sci(sep[i]);
    }

    int n_actions = 0;
    if (config.oracle == "arceager")
        n_actions = (config.labeled) ? (known_labels.size() * 4 - 4) : 7;// attach system
    else if (config.oracle == "listsystem")
        n_actions = (config.labeled) ? (known_labels.size() * 3 - 3) : 5;// list system

    Mat<double> W2(n_actions, h_size);
    for (int j = 0; j < W2.ncols(); ++j)
    {
        getline(input, s);
        vector<string> sep = split(s);
        assert (sep.size() == n_actions);
        for (int i = 0; i < W2.nrows(); ++i)
            W2[i][j] = to_double_sci(sep[i]);
    }

    pre_computed_ids.clear();
    while (pre_computed_ids.size() < (size_t)n_pre_computed)
    {
        getline(input, s);
        vector<string> sep = split(s);
        for (size_t i = 0; i < sep.size(); ++i)
            pre_computed_ids.push_back(to_int(sep[i]));
    }

    input.close();
    if (re_precompute)
        classifier = new NNClassifier(config, Eb, Ed, Ev, Ec, El, W1, b1, W2, vector<int>());
    else
        classifier = new NNClassifier(config, Eb, Ed, Ev, Ec, El, W1, b1, W2, pre_computed_ids);

    vector<string> ldict = known_labels;
    if (config.labeled) ldict.pop_back(); // remove the NIL label
    if (config.oracle == "arceager")
        system =  new ArcEager(ldict, config.language, config.labeled);
    else if (config.oracle == "listsystem")
        system =  new ListSystem(ldict, config.language, config.labeled);

    if (!re_precompute && config.num_pre_computed > 0)
        classifier->pre_compute();

    double end = get_time();
    cerr << "Elapsed " << (end - start) << "s\n";
}

void DependencyParser::load_model(const string & filename, bool re_precompute)
{
    load_model(filename.c_str(), re_precompute);
}

/**
 * load model trained in source language
 *  and embeddings from target language
 *
 * NB: only used in testing time
 */
void DependencyParser::load_model_cl(
        const char * filename,
        const char * clemb)
{
    cerr << "Loading (SRC) depparse model from " << filename << endl;

    double start = get_time();

    ifstream input(filename);

    string s;
    getline(input, s); int n_dict = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_pos = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_label = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_dist = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_valency = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_cluster = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_length = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int Eb_size = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int Ed_size = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int Ev_size = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int Ec_size = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int El_size = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int h_size = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_basic_tokens = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_dist_tokens = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_valency_tokens = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_cluster_tokens = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_length_tokens = to_int(split_by_sep(s, "=")[1]);
    getline(input, s); int n_pre_computed = to_int(split_by_sep(s, "=")[1]);

    // verification
    if (n_dist_tokens == 0) assert (n_dist == 0);
    if (n_valency_tokens == 0) assert (n_valency == 0);
    if (n_cluster_tokens == 0) assert (n_cluster == 0);
    if (n_length_tokens == 0) assert (n_length == 0);

    known_words.clear();
    known_poss.clear();
    known_labels.clear();
    known_distances.clear();
    known_valencies.clear();
    known_clusters.clear();
    known_lengths.clear();

    read_embed_file(clemb);

    int n_cldict = embed_ids.size(); // vocab size of target language

    int index = 0;

    int Eb_entries = n_cldict + 3 + n_label; // 3 -> -UNKNOWN-, -NULL-, -ROOT-
    if (config.use_postag) Eb_entries += n_pos;
    int Ed_entries = n_dist;
    int Ev_entries = n_valency;
    int Ec_entries = n_cluster;
    int El_entries = n_length;

    Mat<double> Eb(Eb_entries, Eb_size);
    Mat<double> Ed(Ed_entries, Ed_size);
    Mat<double> Ev(Ev_entries, Ev_size);
    Mat<double> Ec(Ec_entries, Ec_size);
    Mat<double> El(El_entries, El_size);

    // unordered_map<string, int>::iterator iter = embed_ids.begin();
    auto iter = embed_ids.begin();
    for (; iter != embed_ids.end(); ++iter)
    {
        string word = iter->first;
        known_words.push_back(word);

        assert (embeddings.ncols() == Eb_size);
        for (int j = 0; j < Eb_size; ++j)
            Eb[index][j] = embeddings[iter->second][j];
        index += 1;
    }

    cerr << "index = " << index << endl;
    cerr << "n_cldict = " << n_cldict << endl;
    // assert (index == n_cldict); // not necessary
    assert (n_cldict == (int)known_words.size()); // debug

    for (int i = 0; i < n_dict; ++i)
    {
        getline(input, s);
        vector<string> sep = split(s);

        assert (sep.size() == Eb_size + 1);
        assert (config.embedding_size == Eb_size);
        if (sep[0] == Config::UNKNOWN
                || sep[0] == Config::ROOT
                || sep[0] == Config::NIL)
        {
            known_words.push_back(sep[0]);
            for (int j = 0; j < Eb_size; ++j)
                Eb[index][j] = to_double_sci(sep[j+1]);
            index += 1;
        }
        // else: skip all words in source language
    }

    assert (index == n_cldict + 3); // debug

    if (config.use_postag)
        for (int i = 0; i < n_pos; ++i)
        {
            getline(input, s);
            vector<string> sep = split(s);
            known_poss.push_back(sep[0]);

            assert (sep.size() == Eb_size + 1);
            for (int j = 0; j < Eb_size; ++j)
                Eb[index][j] = to_double_sci(sep[j+1]);
            index += 1;
        }
    if (config.labeled) // always true
        for (int i = 0; i < n_label; ++i)
        {
            getline(input, s);
            vector<string> sep = split(s);
            known_labels.push_back(sep[0]);

            assert (sep.size() == Eb_size + 1);
            for (int j = 0; j < Eb_size; ++j)
                Eb[index][j] = to_double_sci(sep[j+1]);
            index += 1;
        }
    else // always false
    {
        known_labels.push_back(Config::UNKNOWN);
    }

    index = 0; // reset
    if (config.use_distance)  // or (n_dist_tokens > 0)
        for (int i = 0; i < n_dist; ++i)
        {
            getline(input, s);
            vector<string> sep = split(s);
            known_distances.push_back(to_int(sep[0]));

            assert (sep.size() == Ed_size + 1);
            assert (config.distance_embedding_size == Ed_size);
            for (int j = 0; j < Ed_size; ++j)
                Ed[index][j] = to_double_sci(sep[j+1]);
            index += 1;
        }

    index = 0; // reset
    if (config.use_valency)  // or (n_valency_tokens > 0)
        for (int i = 0; i < n_valency; ++i)
        {
            getline(input, s);
            vector<string> sep = split(s);
            known_valencies.push_back(sep[0]);

            assert (sep.size() == Ev_size + 1);
            assert (config.valency_embedding_size == Ev_size);
            for (int j = 0; j < Ev_size; ++j)
                Ev[index][j] = to_double_sci(sep[j+1]);
            index += 1;
        }

    index = 0; // reset
    if (config.use_cluster)  // or (n_cluster_tokens > 0)
        for (int i = 0; i < n_cluster; ++i)
        {
            getline(input, s);
            vector<string> sep = split(s);
            known_clusters.push_back(sep[0]);

            assert (sep.size() == Ec_size + 1);
            assert (config.cluster_embedding_size == Ec_size);
            /*
            if (sep[0] == Config::UNKNOWN)
                for (int j = 0; j < Ec_size; ++j)
                    Ec[index][j] = 0.0;
            else
            */
                for (int j = 0; j < Ec_size; ++j)
                    Ec[index][j] = to_double_sci(sep[j+1]);
            index += 1;
        }

    index = 0; // reset
    if (config.use_length) 
        for (int i = 0; i < n_length; ++i)
        {
            getline(input, s);
            vector<string> sep = split(s);
            known_lengths.push_back(to_int(sep[0]));

            assert (sep.size() == Ed_size + 1);
            assert (config.length_embedding_size == El_size);
            for (int j = 0; j < El_size; ++j)
                El[index][j] = to_double_sci(sep[j+1]);
            index += 1;
        }

    generate_ids();

    int W1_ncol = Eb_size * n_basic_tokens
                + Ed_size * n_dist_tokens
                + Ev_size * n_valency_tokens
                + Ec_size * n_cluster_tokens
                + El_size * n_length_tokens;

    Mat<double> W1(h_size, W1_ncol);
    for (int j = 0; j < W1.ncols(); ++j)
    {
        getline(input, s);
        vector<string> sep = split(s);

        assert (sep.size() == h_size);
        for (int i = 0; i < W1.nrows(); ++i)
            W1[i][j] = to_double_sci(sep[i]);
    }

    Vec<double> b1(h_size);
    getline(input, s);
    vector<string> sep = split(s);

    assert (sep.size() == h_size);
    for (int i = 0; i < b1.size(); ++i)
    {
        b1[i] = to_double_sci(sep[i]);
    }

    int n_actions = 0;
    if (config.oracle == "arceager")
        n_actions = (config.labeled) ? (known_labels.size() * 4 - 4) : 7;// attach system
    else if (config.oracle == "listsystem")
        n_actions = (config.labeled) ? (known_labels.size() * 3 - 3) : 5;// attach system

    Mat<double> W2(n_actions, h_size);
    for (int j = 0; j < W2.ncols(); ++j)
    {
        getline(input, s);
        vector<string> sep = split(s);
        assert (sep.size() == n_actions);
        for (int i = 0; i < W2.nrows(); ++i)
            W2[i][j] = to_double_sci(sep[i]);
    }

    pre_computed_ids.clear();
    while (pre_computed_ids.size() < (size_t)n_pre_computed)
    {
        getline(input, s);
        vector<string> sep = split(s);
        for (size_t i = 0; i < sep.size(); ++i)
            pre_computed_ids.push_back(to_int(sep[i]));
    }

    input.close();
    classifier = new NNClassifier(config, Eb, Ed, Ev, Ec, El, W1, b1, W2, vector<int>());
    vector<string> ldict = known_labels;
    if (config.labeled)
        ldict.pop_back(); // remove the NIL label
    if (config.oracle == "arceager")
        system =  new ArcEager(ldict, config.language, config.labeled);
    else if (config.oracle == "listsystem")
        system =  new ListSystem(ldict, config.language, config.labeled);

    /*
    if (config.num_pre_computed > 0)
        classifier->pre_compute();
    */

    double end = get_time();
    cerr << "Elapsed " << (end - start) << "s\n";
}

void DependencyParser::load_model_cl(const string & filename, const string & clemb)
{
    load_model_cl(filename.c_str(), clemb.c_str());
}

void DependencyParser::test(
        const char * test_file,
        const char * output_file,
        bool re_precompute)
{
    // predict
    cerr << "Test file: " << test_file << endl;

    double start = get_time();

    vector<DependencySent> test_sents;
    vector<DependencyGraph> test_graphs;
    Util::load_conll_file_graph(test_file, test_sents, test_graphs);
    //Util::print_tree_stats(test_trees);

    if (re_precompute)
    {
        cerr << "Pre-computing based on test file (Oracle)" << endl;
        vector<int> test_precompute_ids;
        scan_test_samples(test_sents, test_graphs, test_precompute_ids);
        cerr << "test_precompute_ids.size = " << test_precompute_ids.size() << endl;
        classifier->pre_compute(test_precompute_ids, true);
    }

    int n_words = 0;
    int n_sents = test_sents.size();
    for (size_t i = 0; i < test_sents.size(); ++i)
        n_words += test_sents[i].n;

    vector<DependencyGraph> predicted;
    predict_graph(test_sents, predicted);

    map<string, double> result;
    system->evaluate(test_sents, predicted, test_graphs, result);
   // double las_wo_punc = result["LASwoPunc"];
    //double uas_wo_punc = result["UASwoPunc"];
   // double uas_sub_obj = result["SOUAS"];

    double LF = result["LF"];
    double UF = result["UF"];
    double NUF = result["NUF"];
    double NLF = result["NLF"];

    fprintf(stderr, "UF = %.4f%%\n", UF);
    fprintf(stderr, "LF = %.4f%%\n", LF);
    fprintf(stderr, "NUF = %.4f%%\n", NUF);
    fprintf(stderr, "NLF = %.4f%%\n", NLF);
    //fprintf(stderr, "SUB/OBJ-UAS = %.4f%%\n", uas_sub_obj);

    double end = get_time();

    double wordspersec = n_words / (end - start);
    double sentspersec = n_sents / (end - start);

    fprintf(stderr, "%.1f words per second.\n", wordspersec);
    fprintf(stderr, "%.1f sents per second.\n", sentspersec);
    
    if (output_file != NULL)
        Util::write_conll_file_graph(output_file, test_sents, predicted);
}

void DependencyParser::test(
        string & test_file,
        string & output_file,
        bool re_precompute)
{
    test(test_file.c_str(), output_file.c_str(), re_precompute);
}

void DependencyParser::process_headless(Configuration& c)
{   
    //cerr << "headless" <<endl;
    string root_label = known_labels[known_labels.size() - 2]; ///???is this root label?
    for (int i = 1; i <= c.graph.n; i++){
        //cerr << "i:" << i <<endl;
        if (!c.has_head(i) && !c.find_2nd_head(i)){
            //cerr << "search"<< endl;
            vector<Snd_head> cand_2nd_heads(c.graph.n);
            process_headless_search_all(i, cand_2nd_heads, c, 1); // node after i
            process_headless_search_all(i, cand_2nd_heads, c, -1); // node before i
            Snd_head opt_head;
            opt_head.score = -DBL_MAX;
            opt_head.head = -1;
            for (int j = 0; j < c.graph.n; j++){
                if (cand_2nd_heads[j].score > opt_head.score
                    && !c.has_path_to(i, cand_2nd_heads[j].head)) // no cycle
                    opt_head = cand_2nd_heads[j];
            }
            if (opt_head.head != -1)
                c.add_arc(opt_head.head, i, opt_head.label);
            else
                c.add_arc(0, i, root_label);
        }
    }
}

void DependencyParser::process_headless_search_all(int k, vector<Snd_head>& cand_2nd_heads, Configuration& c, int dir)
{
    int graph_size = c.graph.n;
    for (int i = k + dir; i > 0 && i <= graph_size; i += dir){
        if (dir > 0)
            c.reset(k, i);
        else
            c.reset(i, k);
        double opt_score;
        string opt_label;
        get_best_label(c, opt_label, opt_score, dir);
        Snd_head snd_head;
        snd_head.head = i; snd_head.label = opt_label; snd_head.score = opt_score;
        cand_2nd_heads[i - 1] = snd_head; // head i is stored in [i-1]
    }
}

void DependencyParser::get_best_label(Configuration c, string & opt_label, double & opt_score, int arc_dir)
{
    string prefix = arc_dir > 0 ? "L" : "R";
    int num_trans = system->transitions.size();
    vector<double> scores;
    vector<int> features = get_features(c);
    classifier->compute_scores(features, scores);

    opt_score = -DBL_MAX;
    string opt_trans = "";

    for (int i = 0; i < num_trans; ++i){
        if (startswith(system->transitions[i], prefix) //ensure the arc is in right direction
            &&scores[i] > opt_score){
            opt_score = scores[i];
            opt_trans = system->transitions[i];
        }
    }

    opt_label = opt_trans.substr(3, opt_trans.length() - 4);
}