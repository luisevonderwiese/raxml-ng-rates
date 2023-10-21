#include "Optimizer.hpp"
#include "topology/RFDistCalculator.hpp"

using namespace std;

Optimizer::Optimizer (const Options &opts) :
    _lh_epsilon(opts.lh_epsilon), _lh_epsilon_brlen_triplet(opts.lh_epsilon_brlen_triplet), 
    _spr_radius(opts.spr_radius), _spr_cutoff(opts.spr_cutoff), 
    _nni_epsilon(opts.nni_epsilon), _nni_tolerance(opts.nni_tolerance)
{
}

Optimizer::~Optimizer ()
{
  // TODO Auto-generated destructor stub
}

double Optimizer::optimize_model(TreeInfo& treeinfo, double lh_epsilon)
{
  double new_loglh = treeinfo.loglh();

//  if (!params_to_optimize)
//    return new_loglh;

  int iter_num = 0;
  double cur_loglh;
  do
  {
    cur_loglh = new_loglh;

    treeinfo.optimize_params_all(lh_epsilon);

    new_loglh = treeinfo.loglh();

//      printf("old: %f, new: %f\n", cur_loglh, new_loglh);

    iter_num++;
    LOG_DEBUG << "Iteration " << iter_num <<  ": logLH = " << new_loglh << endl;
  }
  while (new_loglh - cur_loglh > lh_epsilon);

  return new_loglh;
}

void Optimizer::nni(TreeInfo& treeinfo, nni_round_params& nni_params, double& loglh){
  // nni round
  LOG_PROGRESS(loglh) << "NNI round tolerance = " <<  nni_params.tolerance << ", epsilon = " << nni_params.lh_epsilon << endl;
  loglh = treeinfo.nni_round(nni_params);
  
}

double Optimizer::optimize_topology(TreeInfo& treeinfo, CheckpointManager& cm)
{
  const double fast_modopt_eps = 10.;
  const double interim_modopt_eps = 3.;
  const double final_modopt_eps = 0.1;

  SearchState local_search_state = cm.search_state();
  auto& search_state = ParallelContext::group_master_thread() ? cm.search_state() : local_search_state;
  ParallelContext::barrier();

  /* set references such that we can work directly with checkpoint values */
  double &loglh = search_state.loglh;
  int& iter = search_state.iteration;
  spr_round_params& spr_params = search_state.spr_params;
  int& best_fast_radius = search_state.fast_spr_radius;

  spr_params.lh_epsilon_brlen_full = _lh_epsilon;
  spr_params.lh_epsilon_brlen_triplet = _lh_epsilon_brlen_triplet;

  CheckpointStep resume_step = search_state.step;

  /* Compute initial LH of the starting tree */
  loglh = treeinfo.loglh();

  auto do_step = [&search_state,resume_step](CheckpointStep step) -> bool
      {
        if (step >= resume_step)
        {
          search_state.step = step;
          return true;
        }
        else
          return false;;
      };

  if (do_step(CheckpointStep::brlenOpt))
  {
    cm.update_and_write(treeinfo);
    LOG_PROGRESS(loglh) << "Initial branch length optimization" << endl;
    loglh = treeinfo.optimize_branches(fast_modopt_eps, 1);
  }

  /* Initial fast model optimization */
  if (do_step(CheckpointStep::modOpt1))
  {
    cm.update_and_write(treeinfo);
    LOG_PROGRESS(loglh) << "Model parameter optimization (eps = " << fast_modopt_eps << ")" << endl;
    loglh = optimize_model(treeinfo, fast_modopt_eps);

    /* start spr rounds from the beginning */
    iter = 0;
  }

  // do SPRs
  const int radius_limit = min(22, (int) treeinfo.pll_treeinfo().tip_count - 3 );
  const int radius_step = 5;

//  treeinfo->counter = 0;

  if (_spr_radius > 0)
    best_fast_radius = _spr_radius;
  else
  {
    /* auto detect best radius for fast SPRs */

    if (do_step(CheckpointStep::radiusDetectOrNNI))
    {
      if (iter == 0)
      {
        spr_params.thorough = 0;
        spr_params.radius_min = 1;
        best_fast_radius = spr_params.radius_max = 5;
        spr_params.ntopol_keep = 0;
        spr_params.subtree_cutoff = 0.;
      }

      double best_loglh = loglh;

      while (spr_params.radius_min < radius_limit)
      {
        cm.update_and_write(treeinfo);

        ++iter;
        LOG_PROGRESS(best_loglh) << "AUTODETECT spr round " << iter << " (radius: " <<
            spr_params.radius_max << ")" << endl;
        loglh = treeinfo.spr_round(spr_params);

        if (loglh - best_loglh > 0.1)
        {
          /* LH improved, try to increase the radius */
          best_fast_radius = spr_params.radius_max;
          spr_params.radius_min += radius_step;
          spr_params.radius_max += radius_step;
          best_loglh = loglh;
        }
        else
          break;
      }
    }
  }

  LOG_PROGRESS(loglh) << "SPR radius for FAST iterations: " << best_fast_radius << " (" <<
                 (_spr_radius > 0 ? "user-specified" : "autodetect") << ")" << endl;

  if (do_step(CheckpointStep::modOpt2))
  {
    cm.update_and_write(treeinfo);

    /* optimize model parameters a bit more thoroughly */
    LOG_PROGRESS(loglh) << "Model parameter optimization (eps = " <<
                                                            interim_modopt_eps << ")" << endl;
    loglh = optimize_model(treeinfo, interim_modopt_eps);

    /* reset iteration counter for fast SPRs */
    iter = 0;

    /* initialize search params */
    spr_params.thorough = 0;
    spr_params.radius_min = 1;
    spr_params.radius_max = best_fast_radius;
    spr_params.ntopol_keep = 20;
    spr_params.subtree_cutoff = _spr_cutoff;
    spr_params.reset_cutoff_info(loglh);
  }

  double old_loglh;

  if (do_step(CheckpointStep::fastSPR))
  {
    do
    {
      cm.update_and_write(treeinfo);
      ++iter;
      old_loglh = loglh;
      LOG_PROGRESS(old_loglh) << (spr_params.thorough ? "SLOW" : "FAST") <<
          " spr round " << iter << " (radius: " << spr_params.radius_max << ")" << endl;
      loglh = treeinfo.spr_round(spr_params);

      /* optimize ALL branches */
      loglh = treeinfo.optimize_branches(_lh_epsilon, 1);
    }
    while (loglh - old_loglh > _lh_epsilon);
  }

  if (do_step(CheckpointStep::modOpt3))
  {
    cm.update_and_write(treeinfo);
    LOG_PROGRESS(loglh) << "Model parameter optimization (eps = " << 1.0 << ")" << endl;
    loglh = optimize_model(treeinfo, 1.0);

    /* init slow SPRs */
    spr_params.thorough = 1;
    spr_params.radius_min = 1;
    spr_params.radius_max = radius_step;
    iter = 0;
  }

  if (do_step(CheckpointStep::slowSPR))
  {
    do
    {
      cm.update_and_write(treeinfo);
      ++iter;
      old_loglh = loglh;
      LOG_PROGRESS(old_loglh) << (spr_params.thorough ? "SLOW" : "FAST") <<
          " spr round " << iter << " (radius: " << spr_params.radius_max << ")" << endl;
      loglh = treeinfo.spr_round(spr_params);

      /* optimize ALL branches */
      loglh = treeinfo.optimize_branches(_lh_epsilon, 1);

      bool impr = (loglh - old_loglh > _lh_epsilon);
      if (impr)
      {
        /* got improvement in thorough mode: reset min radius to 1 */
        spr_params.radius_min = 1;
        /* reset max radius to 5; or maybe better keep old value? */
        spr_params.radius_max = radius_step;
      }
      else
      {
        /* no improvement in thorough mode: set min radius to old max,
         * and increase max radius by the step */
        spr_params.radius_min = spr_params.radius_max + 1;
        spr_params.radius_max += radius_step;
      }
    }
    while (spr_params.radius_min >= 0 && spr_params.radius_min < radius_limit);
  }

  /* Final thorough model optimization */
  if (do_step(CheckpointStep::modOpt4))
  {
    cm.update_and_write(treeinfo);
    LOG_PROGRESS(loglh) << "Model parameter optimization (eps = " << final_modopt_eps << ")" << endl;
    loglh = optimize_model(treeinfo, final_modopt_eps);
  }

  if (do_step(CheckpointStep::finish))
    cm.update_and_write(treeinfo);

  return loglh;
}

double Optimizer::optimize_topology_adaptive(TreeInfo& treeinfo, CheckpointManager& cm, double difficulty)
{
  // TODO: connect the command line arguments for nni-epsilon and nni-tolerance with nni_params.lh_epsilon and 
  // nni_params.tolerance
  const double fast_modopt_eps = 10.;
  const double interim_modopt_eps = 3.;
  const double final_modopt_eps = 0.1;

  SearchState local_search_state = cm.search_state();
  auto& search_state = ParallelContext::group_master_thread() ? cm.search_state() : local_search_state;
  ParallelContext::barrier();

  /* set references such that we can work directly with checkpoint values */
  double &loglh = search_state.loglh;
  int& iter = search_state.iteration;

  // spr round - basics
  spr_round_params& spr_params = search_state.spr_params;

  // spr parameters - basics
  spr_params.lh_epsilon_brlen_full = _lh_epsilon;
  spr_params.lh_epsilon_brlen_triplet = _lh_epsilon_brlen_triplet;

  // nni round - basics
  nni_round_params& nni_params = search_state.nni_params;
  nni_params.tolerance = _nni_tolerance;
  nni_params.lh_epsilon = _nni_epsilon;

  bool easy_or_difficult = (difficulty <= 0.3) || (difficulty >= 0.7);

  CheckpointStep resume_step = search_state.step;

  /* Compute initial LH of the starting tree */
  loglh = treeinfo.loglh();

  auto do_step = [&search_state,resume_step](CheckpointStep step) -> bool
      {
        if (step >= resume_step)
        {
          search_state.step = step;
          return true;
        }
        else
          return false;;
      };

  /* Compute initial LH of the starting tree */
  loglh = treeinfo.loglh();

  /* Initial branch-length model optimization */
  if (do_step(CheckpointStep::brlenOpt))
  {
    cm.update_and_write(treeinfo);
    LOG_PROGRESS(loglh) << "Initial branch length optimization" << endl;
    loglh = treeinfo.optimize_branches(fast_modopt_eps, 1);
  }

  /* Initial fast model optimization */
  if (do_step(CheckpointStep::modOpt1))
  {
    cm.update_and_write(treeinfo);
    LOG_PROGRESS(loglh) << "Model parameter optimization (eps = " << fast_modopt_eps << ")" << endl;
    loglh = optimize_model(treeinfo, fast_modopt_eps);

    /* start spr rounds from the beginning */
    iter = 0;
  }

  // If the dataset is "easy" or "difficult", start with an NNI round
  if (do_step(CheckpointStep::radiusDetectOrNNI)){
    cm.update_and_write(treeinfo);
    if (easy_or_difficult) nni(treeinfo, nni_params, loglh);
  }
  
  // + model parameter optimization
  if (do_step(CheckpointStep::modOpt2)){  
    cm.update_and_write(treeinfo);
    
    if(easy_or_difficult){
      LOG_PROGRESS(loglh) << "Model parameter optimization (eps = " << interim_modopt_eps << ")" << endl;
      loglh = optimize_model(treeinfo, fast_modopt_eps);
    }
  }

  // do SPRs
  const int radius_limit = min(spr_radius_limit_adaptive(difficulty), (int) treeinfo.pll_treeinfo().tip_count - 3);
  int radius_step = spr_radius_step_adaptive(radius_limit, false);
  double old_loglh;
  bool impr = true, condition = true;
  double impr_perc = 1;

  // setting up fast SPR parameters
  if(do_step(CheckpointStep::fastSPR)){
    
    if(iter == 0){
      spr_params.thorough = 0;
      spr_params.radius_min = 1;
      spr_params.radius_max = radius_step;
      spr_params.ntopol_keep = 0;
      spr_params.subtree_cutoff = 0.;

    }
    
    /* Fast SPR-NNI rounds */
    while(condition) {

      cm.update_and_write(treeinfo);
      ++iter;
      old_loglh = loglh;
      
      // spr round
      LOG_PROGRESS(loglh) << "SPR round " << iter << " (radius: " <<
          spr_params.radius_max << ")" << endl;
      loglh = treeinfo.spr_round(spr_params);
      
      // nni round
      if (spr_params.radius_max > 2*radius_step) nni(treeinfo, nni_params, loglh);

      impr_perc = (loglh - old_loglh) / fabs(loglh);
      impr = (loglh - old_loglh > _lh_epsilon);
      condition = impr && (impr_perc >= 1e-3);

      if((!condition) &&
        (!easy_or_difficult) && 
        (spr_params.radius_max == radius_step) &&
        (radius_step < radius_limit))
      {
        spr_params.radius_min += radius_step;
        spr_params.radius_max += radius_step;
        condition = true;
        continue;
      }

      if (impr_perc <= 0.01 && (spr_params.radius_min + radius_step < radius_limit))
      {
        spr_params.radius_min += radius_step;
        spr_params.radius_max += radius_step;
      }     
    }
  }
  
  if (do_step(CheckpointStep::modOpt3)){
    cm.update_and_write(treeinfo);

    /* optimize model parameters a bit more thoroughly */
    LOG_PROGRESS(loglh) << "Model parameter optimization (eps = " <<
                                                            interim_modopt_eps << ")" << endl;
    loglh = optimize_model(treeinfo, interim_modopt_eps);

    // slow spr setup
    radius_step = spr_radius_step_adaptive(radius_limit, true);
    iter = 0;
    spr_params.thorough = 1;
    spr_params.radius_min = 1;
    spr_params.radius_max = radius_step;
    spr_params.ntopol_keep = 20;
    spr_params.subtree_cutoff = _spr_cutoff;
    spr_params.reset_cutoff_info(loglh);

  }

  if (do_step(CheckpointStep::slowSPR))
  {
    do
    {
      cm.update_and_write(treeinfo);
      ++iter;
      old_loglh = loglh;

      LOG_PROGRESS(old_loglh) << (spr_params.thorough ? "SLOW" : "FAST") <<
          " spr round " << iter << " (radius: " << spr_params.radius_max << ")" << endl;
      loglh = treeinfo.spr_round(spr_params);
      
      if(spr_params.radius_min > radius_step) nni(treeinfo, nni_params, loglh);

      /* optimize ALL branches */
      loglh = treeinfo.optimize_branches(_lh_epsilon, 1);
      
      impr = (loglh - old_loglh > _lh_epsilon);
      
      impr_perc = (loglh - old_loglh) / fabs(loglh);
      
      if ((!impr) || 
          (spr_params.radius_min + radius_step < radius_limit && impr_perc <= 1e-3))
      {
        spr_params.radius_min = spr_params.radius_max + 1;
        spr_params.radius_max += radius_step;
      }

      /* if (impr)
      {
        spr_params.radius_min -= 1;
        spr_params.radius_max = radius_step;
      }
      else
      {
        spr_params.radius_min = spr_params.radius_max + 1;
        spr_params.radius_max += radius_step;
      } */
    }
    while (spr_params.radius_min < radius_limit); 
  }

  if (do_step(CheckpointStep::modOpt4))
  {
    cm.update_and_write(treeinfo);
    LOG_PROGRESS(loglh) << "Model parameter optimization (eps = " << final_modopt_eps << ")" << endl;
    loglh = optimize_model(treeinfo, final_modopt_eps);
  }

  if (do_step(CheckpointStep::finish))
    cm.update_and_write(treeinfo);

  return loglh;
}

double Optimizer::evaluate(TreeInfo& treeinfo, CheckpointManager& cm)
{
  const double fast_modopt_eps = 10.;

  SearchState local_search_state = cm.search_state();
  auto& search_state = ParallelContext::group_master_thread() ? cm.search_state() : local_search_state;
  ParallelContext::barrier();

  double &loglh = search_state.loglh;

  /* Compute initial LH of the starting tree */
  loglh = treeinfo.loglh();

  CheckpointStep resume_step = search_state.step;
  auto do_step = [&search_state,resume_step](CheckpointStep step) -> bool
      {
        if (step >= resume_step)
        {
          search_state.step = step;
          return true;
        }
        else
          return false;;
      };

  if (do_step(CheckpointStep::brlenOpt))
  {
    cm.update_and_write(treeinfo);
    LOG_PROGRESS(loglh) << "Initial branch length optimization" << endl;
    loglh = treeinfo.optimize_branches(fast_modopt_eps, 1);
  }

  /* Model optimization */
  if (do_step(CheckpointStep::modOpt1))
  {
    cm.update_and_write(treeinfo);
    LOG_PROGRESS(loglh) << "Model parameter optimization (eps = " << _lh_epsilon << ")" << endl;
    loglh = optimize_model(treeinfo);
  }

  if (do_step(CheckpointStep::finish))
    cm.update_and_write(treeinfo);

  return loglh;
}

int Optimizer::spr_radius_limit_adaptive(double difficulty){
  if (difficulty <= 0.5){
    return (int) (30*difficulty + 5);
  } else {
    return (int) (-30*difficulty + 35);
  }
}

int Optimizer::spr_radius_step_adaptive(int spr_radius_max, bool slow_spr){

  int radius_step = 0;
  if(slow_spr)
  {
    if(spr_radius_max <= 7){
      radius_step = spr_radius_max;
    } else if (spr_radius_max <= 13){
      radius_step = (int) ((spr_radius_max / 2) + 1);
    } else {
      radius_step = (int) ((spr_radius_max / 3) + 1);
    }

  } 
  else 
  {
    if(spr_radius_max <= 5){
      radius_step = spr_radius_max;
    } else if (spr_radius_max <= 10){
      radius_step = (int) ((spr_radius_max / 2) + 1);
    } else if (spr_radius_max <= 15){
      radius_step = (int) ((spr_radius_max / 3) + 1);
    } else {
      radius_step = (int) ((spr_radius_max / 4) + 1);
    }
  }

  return radius_step;
}