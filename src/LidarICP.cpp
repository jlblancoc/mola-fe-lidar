/* -------------------------------------------------------------------------
 *   A Modular Optimization framework for Localization and mApping  (MOLA)
 * Copyright (C) 2018-2019 Jose Luis Blanco, University of Almeria
 * See LICENSE for license information.
 * ------------------------------------------------------------------------- */
/**
 * @file   LidarICP.cpp
 * @brief  Simple SLAM FrontEnd for point-cloud sensors via ICP registration
 * @author Jose Luis Blanco Claraco
 * @date   Dec 17, 2018
 */

/** \defgroup mola_fe_lidar_icp_grp mola-fe-lidar-icp.
 * Simple SLAM FrontEnd for point-cloud sensors via ICP registration.
 *
 *
 */

#include <mola-fe-lidar-icp/LidarICP.h>
#include <mola-kernel/yaml_helpers.h>
#include <mrpt/core/initializer.h>
#include <mrpt/maps/CSimplePointsMap.h>
#include <mrpt/opengl/COpenGLScene.h>
#include <mrpt/opengl/CText.h>
#include <mrpt/opengl/stock_objects.h>
#include <mrpt/poses/CPose3DPDFGaussian.h>
#include <mrpt/system/datetime.h>
#include <mrpt/system/filesystem.h>
#include <yaml-cpp/yaml.h>

using namespace mola;

MRPT_INITIALIZER(do_register){MOLA_REGISTER_MODULE(LidarICP)}

LidarICP::LidarICP() = default;

void LidarICP::initialize(const std::string& cfg_block)
{
    MRPT_TRY_START

    // Default params:
    params_.mrpt_icp.maxIterations        = 50;
    params_.mrpt_icp.skip_cov_calculation = false;
    params_.mrpt_icp.thresholdDist        = 1.25;
    params_.mrpt_icp.thresholdAng         = mrpt::DEG2RAD(1.0);
    params_.mrpt_icp.ALFA                 = 0.01;

    // Load:
    auto c   = YAML::Load(cfg_block);
    auto cfg = c["params"];
    MRPT_LOG_DEBUG_STREAM("Loading these params:\n" << cfg);

    YAML_LOAD_REQ(params_, min_dist_xyz_between_keyframes, double);
    YAML_LOAD_OPT(params_, min_time_between_scans, double);
    YAML_LOAD_OPT(params_, min_icp_goodness, double);
    YAML_LOAD_OPT(params_, decimate_to_point_count, unsigned int);

    YAML_LOAD_OPT(params_, mrpt_icp.maxIterations, unsigned int);
    YAML_LOAD_OPT(params_, mrpt_icp.thresholdDist, double);
    YAML_LOAD_OPT_DEG(params_, mrpt_icp.thresholdAng, double);

    YAML_LOAD_OPT(params_, debug_save_all_icp_results, bool);

    // attach to world model, if present:
    auto wms = findService<WorldModel>();
    if (wms.size() == 1)
        worldmodel_ = std::dynamic_pointer_cast<WorldModel>(wms[0]);

    MRPT_TRY_END
}
void LidarICP::spinOnce()
{
    MRPT_TRY_START

    ProfilerEntry tleg(profiler_, "spinOnce");

    //
    MRPT_TRY_END
}

void LidarICP::reset() { state_ = MethodState(); }

void LidarICP::onNewObservation(CObservation::Ptr& o)
{
    MRPT_TRY_START
    ProfilerEntry tleg(profiler_, "onNewObservation");

    // Only process "my" sensor source:
    ASSERT_(o);
    if (o->sensorLabel != raw_sensor_label_) return;

    const auto queued = worker_pool_.pendingTasks();
    if (queued > 1)
    {
        MRPT_LOG_THROTTLE_WARN(
            5.0, "Dropping observation due to worker threads too busy.");
        return;
    }
    profiler_.enter("delay_onNewObs_to_process");

    // Enqueue task:
    worker_pool_.enqueue(&LidarICP::doProcessNewObservation, this, o);

    MRPT_TRY_END
}

// here happens the main stuff:
void LidarICP::doProcessNewObservation(CObservation::Ptr& o)
{
    // All methods that are enqueued into a thread pool should have its own
    // top-level try-catch:
    try
    {
        ASSERT_(o);

        ProfilerEntry tleg(profiler_, "doProcessNewObservation");
        profiler_.leave("delay_onNewObs_to_process");

        // Only process pointclouds that are sufficiently apart in time:
        const auto this_obs_tim = o->timestamp;
        if (state_.last_obs_tim != mrpt::Clock::time_point() &&
            mrpt::system::timeDifference(state_.last_obs_tim, this_obs_tim) <
                params_.min_time_between_scans)
        {
            // Drop observation.
            return;
        }

        profiler_.enter("doProcessNewObservation.obs2pointcloud");

        auto       this_obs_points = mrpt::maps::CSimplePointsMap::Create();
        const bool have_points     = this_obs_points->insertObservationPtr(o);

        profiler_.leave("doProcessNewObservation.obs2pointcloud");

        // Store for next step:
        auto last_obs_tim   = state_.last_obs_tim;
        auto last_obs       = state_.last_obs;
        auto last_points    = state_.last_points;
        state_.last_obs     = o;
        state_.last_obs_tim = this_obs_tim;
        state_.last_points  = this_obs_points;

        // First time we cannot do ICP since we need at least two pointclouds:
        if (!last_points)
        {
            MRPT_LOG_DEBUG("First pointcloud: skipping ICP.");
            return;
        }

        if (!have_points)
        {
            MRPT_LOG_WARN_STREAM(
                "Observation of type `" << o->GetRuntimeClass()->className
                                        << "` could not be converted into a "
                                           "pointcloud. Doing nothing.");
            return;
        }

        if (0)
        {
            ProfilerEntry tle(
                profiler_, "doProcessNewObservation.filter_pointclouds");

            this_obs_points->clipOutOfRangeInZ(-1.2f, 5.0f);
            last_points->clipOutOfRangeInZ(-1.2f, 5.0f);
        }

        // Register point clouds using any of the available ICP algorithms:
        mrpt::poses::CPose3DPDFGaussian initial_guess;
        // Use velocity model for the initial guess:
        double dt = .0;
        if (last_obs_tim != mrpt::Clock::time_point())
            dt = mrpt::system::timeDifference(last_obs_tim, this_obs_tim);

        ICP_Output icp_out;
        ICP_Input  icp_in;
        icp_in.init_guess_to_wrt_from = mrpt::math::TPose3D(
            state_.last_iter_twist.vx * dt, state_.last_iter_twist.vy * dt,
            state_.last_iter_twist.vz * dt, 0, 0, 0);
        MRPT_TODO("do omega_xyz part!");

        icp_in.to_pc   = this_obs_points;
        icp_in.from_pc = last_points;
        icp_in.from_id = state_.last_kf;
        icp_in.to_id   = mola::INVALID_ID;  // current data, not a new KF (yet)
        icp_in.debug_str = "lidar_odometry";

        {
            ProfilerEntry tle(profiler_, "doProcessNewObservation.icp_latest");

            run_one_icp(icp_in, icp_out);
        }
        const mrpt::poses::CPose3D rel_pose =
            icp_out.found_pose_to_wrt_from->getMeanVal();

        // Update velocity model:
        state_.last_iter_twist.vx = rel_pose.x() / dt;
        state_.last_iter_twist.vy = rel_pose.y() / dt;
        state_.last_iter_twist.vz = rel_pose.z() / dt;
        MRPT_TODO("do omega_xyz part!");

        MRPT_LOG_DEBUG_STREAM(
            "Cur point count="
            << this_obs_points->size()
            << " last point count=" << last_points->size() << " decimation="
            << params_.mrpt_icp.corresponding_points_decimation);
        MRPT_LOG_DEBUG_STREAM(
            "Est.twist=" << state_.last_iter_twist.asString());
        MRPT_LOG_DEBUG_STREAM(
            "Time since last scan=" << mrpt::system::formatTimeInterval(dt));

        // Create a new KF if the distance since the last one is large enough:
        state_.accum_since_last_kf = state_.accum_since_last_kf + rel_pose;
        const double dist_eucl_since_last = state_.accum_since_last_kf.norm();
        MRPT_TODO("Add rotation threshold");

        MRPT_LOG_DEBUG_FMT(
            "Since last KF: dist=%5.03f m", dist_eucl_since_last);

        // Should we create a new KF?
        if (icp_out.goodness > params_.min_icp_goodness &&
            dist_eucl_since_last > params_.min_dist_xyz_between_keyframes)
        {
            // Yes: create new KF
            // 1) New KeyFrame
            BackEndBase::ProposeKF_Input kf;

            kf.timestamp = this_obs_tim;
            {
                mrpt::obs::CSensoryFrame sf;
                sf.push_back(o);
                kf.observations = std::move(sf);
            }

            std::future<BackEndBase::ProposeKF_Output> kf_out_fut;
            kf_out_fut = slam_backend_->addKeyFrame(kf);

            // Wait until it's executed:
            auto kf_out = kf_out_fut.get();

            ASSERT_(kf_out.success);
            ASSERT_(
                kf_out.new_kf_id &&
                kf_out.new_kf_id.value() != mola::INVALID_ID);

            // Add point cloud to local graph:
            state_.local_pcs[kf_out.new_kf_id.value()] = this_obs_points;

            // 2) New SE(3) constraint between consecutive Keyframes:
            if (state_.last_kf != mola::INVALID_ID)
            {
                std::future<BackEndBase::AddFactor_Output> factor_out_fut;
                mola::FactorRelativePose3                  fPose3(
                    state_.last_kf, kf_out.new_kf_id.value(),
                    state_.accum_since_last_kf.asTPose());

                mola::Factor f = std::move(fPose3);
                factor_out_fut = slam_backend_->addFactor(f);

                // Wait until it's executed:
                auto factor_out = factor_out_fut.get();
                ASSERT_(factor_out.success);
                ASSERT_(
                    factor_out.new_factor_id &&
                    factor_out.new_factor_id != mola::INVALID_FID);

                // Append to local graph as well::
                state_.local_pose_graph.insertEdgeAtEnd(
                    state_.last_kf, *kf_out.new_kf_id,
                    state_.accum_since_last_kf);
            }

            // Done.
            MRPT_LOG_INFO_STREAM(
                "New KF: ID=" << *kf_out.new_kf_id << " rel_pose="
                              << state_.accum_since_last_kf.asString());

            // Reset accumulators:
            state_.accum_since_last_kf = mrpt::poses::CPose3D();
            state_.last_kf             = kf_out.new_kf_id.value();
        }  // end done add a new KF

        // Now, let's try to align this new KF against a few past KFs as well.
        // we'll do it in separate threads, with priorities so the latest KFs
        // are always attended first:
        if (state_.local_pcs.size() > 1)
        {
            ProfilerEntry tle(
                profiler_, "doProcessNewObservation.checkForNearbyKFs");

            checkForNearbyKFs();
        }
    }
    catch (const std::exception& e)
    {
        MRPT_LOG_ERROR_STREAM("Exception:\n" << mrpt::exception_to_str(e));
    }
}

void LidarICP::checkForNearbyKFs()
{
    MRPT_START

    // Run Dijkstra wrt to the last KF:
    auto& lpg = state_.local_pose_graph;

    lpg.root = state_.last_kf;
    lpg.nodes.clear();
    lpg.nodes[lpg.root] = mrpt::poses::CPose3D::Identity();
    lpg.dijkstra_nodes_estimate();

    // Remove too distant KFs: they belong to "loop closure", not to
    // "lidar odometry"!
    std::map<double, mrpt::graphs::TNodeID> KF_distances;
    for (const auto& kfs : lpg.nodes)
        KF_distances[kfs.second.norm()] = kfs.first;

    std::map<mrpt::graphs::TNodeID, std::set<mrpt::graphs::TNodeID>> adj;
    lpg.getAdjacencyMatrix(adj);

    while (lpg.nodes.size() > params_.max_KFs_local_graph)
    {
        const auto id_to_remove = KF_distances.rbegin()->second;
        KF_distances.erase(std::prev(KF_distances.end()));

        lpg.nodes.erase(id_to_remove);
        state_.local_pcs.erase(id_to_remove);
        for (const auto other_id : adj[id_to_remove])
        {
            lpg.edges.erase(std::make_pair(id_to_remove, other_id));
            lpg.edges.erase(std::make_pair(other_id, id_to_remove));
        }
    }

    if (!KF_distances.empty())
    {
        // Pick the node at an intermediary distance and try to align
        // against it:
        auto it = KF_distances.begin();
        std::advance(it, KF_distances.size() * 0.75);
        const auto kf_id = it->second;

        bool edge_already_exists =
            (std::abs(
                 static_cast<int64_t>(kf_id) - static_cast<int64_t>(lpg.root)) <
             2);

        // Already sent out for checking?
        const auto pair_ids = std::make_pair(
            std::min(kf_id, lpg.root), std::max(kf_id, lpg.root));

        if (state_.checked_KF_pairs.count(pair_ids) != 0)
        {
            // Yes:
            edge_already_exists = true;
        }

        MRPT_TODO("Factors should have an annotation to know who created them");
        // Also check in the WorldModel if *we* created an edge already between
        // those two KFs:
        if (!edge_already_exists && worldmodel_)
        {
            worldmodel_->entities_lock();
            const auto connected = worldmodel_->entity_neighbors(kf_id);
            if (connected.count(lpg.root) != 0)
            {
                MRPT_LOG_DEBUG_STREAM(
                    "[checkForNearbyKFs] Discarding pair check since a factor "
                    "already exists between #"
                    << kf_id << " <==> #" << lpg.root);
                edge_already_exists = false;
            }

            worldmodel_->entities_unlock();
        }

        if (!edge_already_exists)
        {
            auto d                    = std::make_shared<ICP_Input>();
            d->to_id                  = kf_id;
            d->from_id                = lpg.root;
            d->to_pc                  = state_.local_pcs[d->to_id];
            d->from_pc                = state_.local_pcs[d->from_id];
            d->init_guess_to_wrt_from = lpg.nodes[kf_id].asTPose();

            worker_pool_past_KFs_.enqueue(
                &LidarICP::doCheckForNonAdjacentKFs, this, d);

            // Mark as sent for check:
            state_.checked_KF_pairs.insert(pair_ids);
        }
    }

    MRPT_END
}

void LidarICP::doCheckForNonAdjacentKFs(const std::shared_ptr<ICP_Input>& d)
{
    try
    {
        ProfilerEntry tleg(profiler_, "doCheckForNonAdjacentKFs");

        // Call ICP:
        // Use current values for: state_.mrpt_icp.options
        mrpt::slam::CICP::TReturnInfo ret_info;
        ASSERT_(d->from_pc);
        ASSERT_(d->to_pc);

        ICP_Output icp_out;
        d->debug_str = "doCheckForNonAdjacentKFs";
        {
            ProfilerEntry tle(profiler_, "doCheckForNonAdjacentKFs.icp");

            run_one_icp(*d, icp_out);
        }
        const mrpt::poses::CPose3D rel_pose =
            icp_out.found_pose_to_wrt_from->getMeanVal();
        const double icp_goodness = icp_out.goodness;

        // Accept the new edge?
        const mrpt::poses::CPose3D init_guess(d->init_guess_to_wrt_from);
        const double pos_correction = (rel_pose - init_guess).norm();
        const double correction_percent =
            pos_correction / (init_guess.norm() + 0.01);

        MRPT_LOG_DEBUG_STREAM(
            "[doCheckForNonAdjacentKFs] Checking KFs: #"
            << d->from_id << " ==> #" << d->to_id
            << " init_guess: " << d->init_guess_to_wrt_from.asString() << "\n"
            << mrpt::format(
                   "MRPT ICP: goodness=%.03f iters=%u\n", ret_info.goodness,
                   ret_info.nIterations)
            << "ICP rel_pose=" << rel_pose.asString() << " init_guess was "
            << init_guess.asString() << " (changes " << 100 * correction_percent
            << "%)");

        if (icp_goodness > params_.min_icp_goodness &&
            correction_percent < 0.20)
        {
            std::future<BackEndBase::AddFactor_Output> factor_out_fut;
            mola::FactorRelativePose3                  fPose3(
                d->from_id, d->to_id, rel_pose.asTPose());

            mola::Factor f = std::move(fPose3);
            factor_out_fut = slam_backend_->addFactor(f);

            // Wait until it's executed:
            auto factor_out = factor_out_fut.get();
            ASSERT_(factor_out.success);
            ASSERT_(
                factor_out.new_factor_id &&
                factor_out.new_factor_id != mola::INVALID_FID);

            // Append to local graph as well::
            state_.local_pose_graph.insertEdgeAtEnd(
                d->from_id, d->to_id, rel_pose);
        }
    }
    catch (const std::exception& e)
    {
        MRPT_LOG_ERROR_STREAM("Exception:\n" << mrpt::exception_to_str(e));
    }
}

void LidarICP::run_one_icp(const ICP_Input& in, ICP_Output& out)
{
    using namespace std::string_literals;

    MRPT_START
    ProfilerEntry tleg(profiler_, "run_one_icp");

    // Call ICP:
    mrpt::slam::CICP mrpt_icp;
    mrpt_icp.options = params_.mrpt_icp;
    if (params_.decimate_to_point_count > 0)
    {
        unsigned decim = static_cast<unsigned>(
            in.to_pc->size() / params_.decimate_to_point_count);
        mrpt_icp.options.corresponding_points_decimation = decim;
    }

    {
        ProfilerEntry tle(profiler_, "run_one_icp.build_kd_tree");

        std::lock_guard<std::mutex> lck(kdtree_build_mtx_);

        // Ensure the kd-tree is built.
        // It's important to enfore it to be build now in a single thread,
        // before the pointclouds go to different threads, which may cause
        // mem corruption:
        in.from_pc->kdTreeEnsureIndexBuilt3D();
        in.to_pc->kdTreeEnsureIndexBuilt3D();
    }

    mrpt::poses::CPose3DPDFGaussian initial_guess;
    initial_guess.mean = mrpt::poses::CPose3D(in.init_guess_to_wrt_from);
    mrpt::slam::CICP::TReturnInfo ret_info;

    out.found_pose_to_wrt_from = mrpt_icp.Align3DPDF(
        in.from_pc.get(), in.to_pc.get(), initial_guess,
        nullptr /*running_time*/, &ret_info);

    out.goodness = static_cast<double>(ret_info.goodness);
    MRPT_LOG_DEBUG_FMT(
        "MRPT ICP: goodness=%.03f iters=%u rel_pose=%s", out.goodness,
        ret_info.nIterations,
        out.found_pose_to_wrt_from->getMeanVal().asString().c_str());

    // -------------------------------------------------
    // Save debug files for debugging ICP quality
    if (params_.debug_save_all_icp_results)
    {
        auto fil_name_prefix = mrpt::system::fileNameStripInvalidChars(
            getModuleInstanceName() +
            mrpt::format(
                "_debug_ICP_%s_%05u", in.debug_str.c_str(),
                state_.debug_dump_icp_file_counter++));

        // Init:
        mrpt::opengl::COpenGLScene scene;

        scene.insert(mrpt::opengl::stock_objects::CornerXYZSimple(2.0f, 4.0f));
        auto gl_from                    = mrpt::opengl::CSetOfObjects::Create();
        in.from_pc->renderOptions.color = mrpt::img::TColorf(.0f, .0f, 1.0f);
        in.from_pc->getAs3DObject(gl_from);
        gl_from->setName("KF_from"s);
        gl_from->enableShowName();
        scene.insert(gl_from);

        auto gl_to = mrpt::opengl::CSetOfObjects::Create();
        gl_to->insert(mrpt::opengl::stock_objects::CornerXYZSimple(1.0f, 2.0f));
        in.to_pc->renderOptions.color = mrpt::img::TColorf(1.0f, .0f, .0f);
        in.to_pc->getAs3DObject(gl_to);
        gl_to->setName("KF_to"s);
        gl_to->enableShowName();
        gl_to->setPose(initial_guess.mean);
        scene.insert(gl_to);

        auto gl_info  = mrpt::opengl::CText::Create(),
             gl_info2 = mrpt::opengl::CText::Create();
        gl_info->setLocation(0., 0., 5.);
        gl_info2->setLocation(0., 0., 4.8);
        scene.insert(gl_info);
        scene.insert(gl_info2);

        {
            std::ostringstream ss;
            ss << "to_ID     = " << in.to_id << " from_ID   = " << in.from_id
               << " | " << in.debug_str;
            gl_info->setString(ss.str());
        }
        {
            std::ostringstream ss;
            ss << "init_pose = " << initial_guess.mean.asString();
            gl_info2->setString(ss.str());
        }

        const auto fil_name_init = fil_name_prefix + "_0init.3Dscene"s;
        if (scene.saveToFile(fil_name_init))
            MRPT_LOG_DEBUG_STREAM(
                "Wrote debug init ICP scene to: " << fil_name_init);
        else
            MRPT_LOG_ERROR_STREAM(
                "Error saving init ICP scene to :" << fil_name_init);

        // Final:
        const auto final_pose = out.found_pose_to_wrt_from->getMeanVal();
        gl_to->setPose(final_pose);

        {
            std::ostringstream ss;
            ss << "to_ID     = " << in.to_id << " from_ID   = " << in.from_id;
            gl_info->setString(ss.str());
        }
        {
            std::ostringstream ss;
            ss << " final_pose = " << final_pose.asString()
               << " goodness: " << out.goodness * 100.0;

            gl_info2->setString(ss.str());
        }

        const auto fil_name_final = fil_name_prefix + "_1final.3Dscene"s;
        if (scene.saveToFile(fil_name_final))
            MRPT_LOG_DEBUG_STREAM(
                "Wrote debug final ICP scene to: " << fil_name_final);
        else
            MRPT_LOG_ERROR_STREAM(
                "Error saving final ICP scene to :" << fil_name_final);
    }

    MRPT_END
}
