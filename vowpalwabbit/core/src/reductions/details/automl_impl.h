// Copyright (c) by respective owners including Yahoo!, Microsoft, and
// individual contributors. All rights reserved. Released under a BSD (revised)
// license as described in the file LICENSE.
#pragma once

#include "vw/core/array_parameters_dense.h"
#include "vw/core/estimator_config.h"
#include "vw/core/learner.h"
#include "vw/core/rand_state.h"

#include <queue>

using namespace VW::config;
using namespace VW::LEARNER;

namespace VW
{
namespace reductions
{
namespace automl
{
namespace
{
constexpr uint64_t MAX_CONFIGS = 10;
constexpr uint64_t CONFIGS_PER_CHAMP_CHANGE = 10;
}  // namespace

using interaction_vec_t = std::vector<std::vector<namespace_index>>;

struct aml_estimator : VW::estimator_config
{
  aml_estimator() : VW::estimator_config() {}
  aml_estimator(double alpha, double tau) : VW::estimator_config(alpha, tau) {}
  aml_estimator(
      VW::estimator_config sc, uint64_t config_index, bool eligible_to_inactivate, interaction_vec_t& live_interactions)
      : VW::estimator_config(sc)
  {
    this->config_index = config_index;
    this->eligible_to_inactivate = eligible_to_inactivate;
    this->live_interactions = live_interactions;
  }
  uint64_t config_index = 0;
  bool eligible_to_inactivate = false;
  interaction_vec_t live_interactions;  // Live pre-allocated vectors in use

  void persist(metric_sink&, const std::string&, bool, const std::string&);
};

// all possible states of exclusion config
enum class config_state
{
  New,
  Live,
  Inactive,
  Removed
};

struct exclusion_config
{
  std::set<std::vector<namespace_index>> exclusions;
  uint64_t lease;
  config_state state = VW::reductions::automl::config_state::New;

  exclusion_config(uint64_t lease = 10) : lease(lease) {}
};

// all possible states of automl
enum class automl_state
{
  Collecting,
  Experimenting
};

struct config_manager
{
  void persist(metric_sink&, bool);
  // config managers own the underlaying weights so they need to know how to clear
  void clear_non_champ_weights();

  // Public Chacha functions
  void schedule();
  void update_champ();
};

using priority_func = float(const exclusion_config&, const std::map<namespace_index, uint64_t>&);

template <typename oracle_impl>
struct config_oracle
{
  std::string _interaction_type;
  const std::string _oracle_type;

  // insert_config(..)
  std::priority_queue<std::pair<float, uint64_t>>& index_queue;
  std::map<namespace_index, uint64_t>& ns_counter;
  std::vector<exclusion_config>& configs;

  priority_func* calc_priority;
  const uint64_t global_lease;
  uint64_t valid_config_size = 0;
  oracle_impl _impl;

  config_oracle(uint64_t global_lease, priority_func* calc_priority,
      std::priority_queue<std::pair<float, uint64_t>>& index_queue, std::map<namespace_index, uint64_t>& ns_counter,
      std::vector<exclusion_config>& configs, const std::string& interaction_type, const std::string& oracle_type,
      std::shared_ptr<VW::rand_state>& rand_state);

  void do_work(std::vector<std::pair<aml_estimator, estimator_config>>& estimators, const uint64_t current_champ);
  void insert_config(std::set<std::vector<namespace_index>>&& new_exclusions, bool allow_dups = false);
  bool repopulate_index_queue();
};

struct oracle_rand_impl
{
  std::shared_ptr<VW::rand_state> random_state;
  oracle_rand_impl(std::shared_ptr<VW::rand_state> random_state) : random_state(std::move(random_state)) {}
  void do_work(config_oracle<oracle_rand_impl>* config_oracle,
      std::vector<std::pair<aml_estimator, estimator_config>>& estimators, const uint64_t current_champ);
};
struct one_diff_impl
{
  void do_work(config_oracle<one_diff_impl>* config_oracle,
      std::vector<std::pair<aml_estimator, estimator_config>>& estimators, const uint64_t current_champ);
};
struct champdupe_impl
{
  void do_work(config_oracle<champdupe_impl>* config_oracle,
      std::vector<std::pair<aml_estimator, estimator_config>>& estimators, const uint64_t current_champ);
};

template <typename config_oracle_impl>
struct interaction_config_manager : config_manager
{
  uint64_t total_champ_switches = 0;
  uint64_t total_learn_count = 0;
  const uint64_t current_champ = 0;
  const uint64_t global_lease;
  const uint64_t max_live_configs;
  uint64_t priority_challengers;
  std::string interaction_type;  // candidate to be removed from here
  dense_parameters& weights;
  double automl_significance_level;
  double automl_estimator_decay;
  VW::io::logger* logger;
  uint32_t& wpp;
  const bool _lb_trick;
  const bool _ccb_on;
  config_oracle_impl _config_oracle;

  // TODO: delete all this, gd and cb_adf must respect ft_offset
  std::vector<double> per_live_model_state_double;
  std::vector<uint64_t> per_live_model_state_uint64;
  double* _gd_normalized = nullptr;
  double* _gd_total_weight = nullptr;
  double* _sd_gravity = nullptr;
  uint64_t* _cb_adf_event_sum = nullptr;
  uint64_t* _cb_adf_action_sum = nullptr;

  // Stores all namespaces currently seen -- Namespace switch could we use array, ask Jack
  std::map<namespace_index, uint64_t> ns_counter;

  // Stores all configs in consideration
  std::vector<exclusion_config> configs;

  // Stores estimators of live configs, size will never exceed max_live_configs. Each pair will be of the form
  // <challenger_estimator, champ_estimator> for the horizon of a given challenger. Thus each challenger has one
  // horizon and the champ has one horizon for each challenger
  std::vector<std::pair<aml_estimator, estimator_config>> estimators;

  // Maybe not needed with oracle, maps priority to config index, unused configs
  std::priority_queue<std::pair<float, uint64_t>> index_queue;

  interaction_config_manager(uint64_t, uint64_t, std::shared_ptr<VW::rand_state>, uint64_t, const std::string&,
      const std::string&, dense_parameters&,
      float (*)(const exclusion_config&, const std::map<namespace_index, uint64_t>&), double, double, VW::io::logger*,
      uint32_t&, bool, bool);

  void do_learning(multi_learner&, multi_ex&, uint64_t);
  void persist(metric_sink&, bool);

  // Public Chacha functions
  void schedule();
  void update_champ();

private:
  static uint64_t choose(std::priority_queue<std::pair<float, uint64_t>>& index_queue);
  static bool swap_eligible_to_inactivate(
      bool lb_trick, std::vector<std::pair<aml_estimator, estimator_config>>& estimators, uint64_t);
};

bool count_namespaces(const multi_ex& ecs, std::map<namespace_index, uint64_t>& ns_counter);
void gen_interactions(bool ccb_on, std::map<namespace_index, uint64_t>& ns_counter, std::string& interaction_type,
    std::vector<exclusion_config>& configs, std::vector<std::pair<aml_estimator, estimator_config>>& estimators,
    uint64_t live_slot);

template <typename CMType>
struct automl
{
  automl_state current_state = automl_state::Collecting;
  std::unique_ptr<CMType> cm;
  VW::io::logger* logger;
  LEARNER::multi_learner* adf_learner = nullptr;  //  re-use print from cb_explore_adf
  bool debug_reverse_learning_order = false;
  const bool should_save_predict_only_model;

  automl(std::unique_ptr<CMType> cm, VW::io::logger* logger, bool predict_only_model)
      : cm(std::move(cm)), logger(logger), should_save_predict_only_model(predict_only_model)
  {
  }
  // This fn gets called before learning any example
  // void one_step(multi_learner&, multi_ex&, CB::cb_class&, uint64_t);
  // template <typename CMType>
  void one_step(multi_learner& base, multi_ex& ec, CB::cb_class& logged, uint64_t labelled_action);
  // inner loop of learn driven by # MAX_CONFIGS
  void offset_learn(multi_learner& base, multi_ex& ec, CB::cb_class& logged, uint64_t labelled_action);

private:
  ACTION_SCORE::action_scores buffer_a_s;  // a sequence of classes with scores.  Also used for probabilities.
};

void apply_config(example* ec, interaction_vec_t* live_interactions);
bool is_allowed_to_remove(const unsigned char ns);
void clear_non_champ_weights(dense_parameters& weights, uint32_t total, uint32_t& wpp);
bool better(bool lb_trick, aml_estimator& challenger, estimator_config& champ);
bool worse();
}  // namespace automl

namespace util
{
void fail_if_enabled(VW::workspace& all, const std::set<std::string>& not_compat);
std::string interaction_vec_t_to_string(
    const VW::reductions::automl::interaction_vec_t& interactions, const std::string& interaction_type);
std::string exclusions_to_string(const std::set<std::vector<VW::namespace_index>>& exclusions);
}  // namespace util
}  // namespace reductions
namespace model_utils
{
template <typename CMType>
size_t write_model_field(io_buf&, const VW::reductions::automl::automl<CMType>&, const std::string&, bool);
size_t read_model_field(io_buf&, VW::reductions::automl::exclusion_config&);
size_t read_model_field(io_buf&, VW::reductions::automl::aml_estimator&);
template <typename config_oracle_impl>
size_t read_model_field(io_buf&, VW::reductions::automl::interaction_config_manager<config_oracle_impl>&);
template <typename CMType>
size_t read_model_field(io_buf&, VW::reductions::automl::automl<CMType>&);
size_t write_model_field(io_buf&, const VW::reductions::automl::exclusion_config&, const std::string&, bool);
size_t write_model_field(io_buf&, const VW::reductions::automl::aml_estimator&, const std::string&, bool);
template <typename config_oracle_impl>
size_t write_model_field(
    io_buf&, const VW::reductions::automl::interaction_config_manager<config_oracle_impl>&, const std::string&, bool);
}  // namespace model_utils
VW::string_view to_string(reductions::automl::automl_state state);
VW::string_view to_string(reductions::automl::config_state state);
}  // namespace VW

namespace fmt
{
template <>
struct formatter<VW::reductions::automl::automl_state> : formatter<std::string>
{
  auto format(VW::reductions::automl::automl_state c, format_context& ctx) -> decltype(ctx.out())
  {
    return formatter<std::string>::format(std::string{VW::to_string(c)}, ctx);
  }
};

template <>
struct formatter<VW::reductions::automl::config_state> : formatter<std::string>
{
  auto format(VW::reductions::automl::config_state c, format_context& ctx) -> decltype(ctx.out())
  {
    return formatter<std::string>::format(std::string{VW::to_string(c)}, ctx);
  }
};
}  // namespace fmt