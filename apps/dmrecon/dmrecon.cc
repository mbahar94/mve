#include <iomanip>
#include <iostream>
#include <cstdlib>
#include <csignal>

#include "dmrecon/settings.h"
#include "dmrecon/dmrecon.h"
#include "mve/scene.h"
#include "mve/view.h"
#include "util/timer.h"
#include "util/arguments.h"
#include "util/system.h"
#include "util/tokenizer.h"
#include "util/file_system.h"

#include "fancy_progress_printer.h"

enum ProgressStyle
{
    PROGRESS_SILENT,
    PROGRESS_SIMPLE,
    PROGRESS_FANCY
};

struct AppSettings
{
    std::string scene_path;
    std::string ply_dest;
    std::string log_dest;
    int master_id;
    std::vector<int> view_ids;
    int max_pixels;
    bool force_recon;
    bool write_ply;
    ProgressStyle progress_style;
    mvs::Settings mvs;
};

FancyProgressPrinter fancyProgressPrinter;

void
reconstruct (mve::Scene::Ptr scene, mvs::Settings settings)
{
    /*
     * Note: destructor of ProgressHandle sets status to failed
     * if setDone() is not called (this happens when an exception
     * is thrown in mvs::DMRecon)
     */
    ProgressHandle handle(fancyProgressPrinter, settings);
    mvs::DMRecon recon(scene, settings);
    handle.setRecon(recon);
    recon.start();
    handle.setDone();
}

void
aabb_from_string (std::string const& str,
    math::Vec3f* aabb_min, math::Vec3f* aabb_max)
{
    util::Tokenizer tok;
    tok.split(str, ',');
    if (tok.size() != 6)
    {
        std::cerr << "Error: Invalid AABB given" << std::endl;
        std::exit(1);
    }

    for (int i = 0; i < 3; ++i)
    {
        (*aabb_min)[i] = tok.get_as<float>(i);
        (*aabb_max)[i] = tok.get_as<float>(i + 3);
    }
    std::cout << "Using AABB: (" << (*aabb_min) << ") / ("
        << (*aabb_max) << ")" << std::endl;
}

int
get_scale_from_max_pixels (mve::Scene::Ptr scene,
    AppSettings const& app_settings, mvs::Settings const& mvs_settings)
{
    mve::View::ConstPtr view = scene->get_view_by_id(mvs_settings.refViewNr);
    if (view == NULL)
        return 0;
    mve::MVEFileProxy const* proxy = view->get_proxy(mvs_settings.imageEmbedding);
    if (proxy == NULL)
        return 0;

    int const width = proxy->width;
    int const height = proxy->height;
    if (width * height <= app_settings.max_pixels)
        return 0;

    float const ratio = width * height / static_cast<float>(app_settings.max_pixels);
    float const scale = std::ceil(std::log(ratio) / std::log(4.0f));

    std::cout << "Setting scale " << scale << " for " << width << "x" << height << " image." << std::endl;

    return std::max(0, static_cast<int>(scale));
}

int
main (int argc, char** argv)
{
    /* Catch segfaults to print stack traces. */
    ::signal(SIGSEGV, util::system::signal_segfault_handler);

    /* Parse arguments. */
    util::Arguments args;
    args.set_usage(argv[0], "[ OPTIONS ] SCENEDIR");
    args.set_helptext_indent(23);
    args.set_nonopt_minnum(1);
    args.set_nonopt_maxnum(1);
    args.set_exit_on_error(true);
    args.add_option('n', "neighbors", true,
        "amount of neighbor views (global view selection)");
    args.add_option('m', "master-view", true,
        "reconstructs given master view ID only");
    args.add_option('l', "list-view", true,
        "reconstructs given view IDs (given as string \"0-10\")");
    args.add_option('s', "scale", true,
        "reconstruction on given scale, 0 is original [0]");
    args.add_option('\0', "max-pixels", true,
        "Limit master image size [disabled]");
    args.add_option('f', "filter-width", true,
        "patch size for NCC based comparison [5]");
    args.add_option('\0', "nocolorscale", false,
        "turn off color scale");
    args.add_option('i', "image", true,
        "specify source image embedding [undistorted]");
    args.add_option('\0', "keep-dz", false,
        "store dz map into view");
    args.add_option('\0', "keep-conf", false,
        "store confidence map into view");
    args.add_option('p', "writeply", false,
        "use this option to write the ply file");
    args.add_option('\0', "plydest", true,
        "path suffix appended to scene dir to write ply files");
    args.add_option('\0', "logdest", true,
        "path suffix appended to scene dir to write log files");
    args.add_option('\0', "bounding-box", true,
        "Six comma separated values used as AABB [disabled]");
    args.add_option('\0', "progress", true,
        "progress output style: 'silent', 'simple' or 'fancy'");
    args.add_option('\0', "force", false,
        "Reconstruct and overwrite existing depthmaps");
    args.parse(argc, argv);

    AppSettings conf;
    conf.scene_path = args.get_nth_nonopt(0);
    conf.ply_dest = "recon";
    conf.log_dest = "log";
    conf.master_id = -1;
    conf.force_recon = false;
    conf.write_ply = false;
    conf.max_pixels = 0;
#ifdef _WIN32
    conf.progress_style = PROGRESS_SIMPLE;
#else
    conf.progress_style = PROGRESS_FANCY;
#endif

    util::ArgResult const* arg;
    while ((arg = args.next_option()))
    {
        if (arg->opt->lopt == "neighbors")
            conf.mvs.globalVSMax = arg->get_arg<std::size_t>();
        else if (arg->opt->lopt == "nocolorscale")
            conf.mvs.useColorScale = false;
        else if (arg->opt->lopt == "scale")
            conf.mvs.scale = arg->get_arg<int>();
        else if (arg->opt->lopt == "filter-width")
            conf.mvs.filterWidth = arg->get_arg<unsigned int>();
        else if (arg->opt->lopt == "image")
            conf.mvs.imageEmbedding = arg->get_arg<std::string>();
        else if (arg->opt->lopt == "keep-dz")
            conf.mvs.keepDzMap = true;
        else if (arg->opt->lopt == "keep-conf")
            conf.mvs.keepConfidenceMap = true;
        else if (arg->opt->lopt == "master-view")
            conf.master_id = arg->get_arg<int>();
        else if (arg->opt->lopt == "list-view")
            args.get_ids_from_string(arg->arg, &conf.view_ids);
        else if (arg->opt->lopt == "writeply")
            conf.write_ply = true;
        else if (arg->opt->lopt == "plydest")
            conf.ply_dest = arg->arg;
        else if (arg->opt->lopt == "logdest")
            conf.log_dest = arg->arg;
        else if (arg->opt->lopt == "max-pixels")
            conf.max_pixels = arg->get_arg<std::size_t>();
        else if (arg->opt->lopt == "bounding-box")
            aabb_from_string(arg->arg, &conf.mvs.aabbMin, &conf.mvs.aabbMax);
        else if (arg->opt->lopt == "progress")
        {
            if (arg->arg == "silent")
                conf.progress_style = PROGRESS_SILENT;
            else if (arg->arg == "simple")
                conf.progress_style = PROGRESS_SIMPLE;
            else if (arg->arg == "fancy")
                conf.progress_style = PROGRESS_FANCY;
            else
            {
                args.generate_helptext(std::cerr);
                std::cerr << "Error: Unrecognized progress style" << std::endl;
                return 1;
            }

        }
        else if (arg->opt->lopt == "force")
            conf.force_recon = true;
        else
        {
            args.generate_helptext(std::cerr);
            std::cerr << "Error: unrecognized option: "
                << arg->opt->lopt << std::endl;
            return 1;
        }
    }

    /* don't show progress twice */
    if (conf.progress_style != PROGRESS_SIMPLE)
        conf.mvs.quiet = true;

    /* Load MVE scene. */
    mve::Scene::Ptr scene = mve::Scene::create();
    try
    {
        scene->load_scene(conf.scene_path);
        scene->get_bundle();
    }
    catch (std::exception& e)
    {
        std::cerr << "Error loading scene: " << e.what() << std::endl;
        return 1;
    }

    /* Settings for Multi-view stereo */
    conf.mvs.writePlyFile = conf.write_ply;
    conf.mvs.plyPath = util::fs::join_path(conf.scene_path, conf.ply_dest);
    conf.mvs.logPath = util::fs::join_path(conf.scene_path, conf.log_dest);

    fancyProgressPrinter.setBasePath(conf.scene_path);
    fancyProgressPrinter.setNumViews(scene->get_views().size());
    if (conf.progress_style == PROGRESS_FANCY)
        fancyProgressPrinter.pt_create();

    util::WallTimer timer;
    if (conf.master_id >= 0)
    {
        /* Calculate scale from max pixels. */
        if (conf.max_pixels > 0)
            conf.mvs.scale = get_scale_from_max_pixels(scene, conf, conf.mvs);

        std::cout << "Reconstructing view ID " << conf.master_id << std::endl;
        conf.mvs.refViewNr = (std::size_t)conf.master_id;
        fancyProgressPrinter.addRefView(conf.master_id);
        try
        {
            reconstruct(scene, conf.mvs);
        }
        catch (std::exception &err)
        {
            std::cerr << err.what() << std::endl;
            return 1;
        }
    }
    else
    {
        mve::Scene::ViewList& views(scene->get_views());
        if (conf.view_ids.empty())
        {
            std::cout << "Reconstructing all views..." << std::endl;
            for(std::size_t i = 0; i < views.size(); ++i)
                conf.view_ids.push_back(i);
        }
        else
        {
            std::cout << "Reconstructing views from list..." << std::endl;
        }
        fancyProgressPrinter.addRefViews(conf.view_ids);

#pragma omp parallel for schedule(dynamic, 1)
        for (std::size_t i = 0; i < conf.view_ids.size(); ++i)
        {
            std::size_t id = conf.view_ids[i];
            if (id >= views.size())
            {
                std::cout << "Invalid ID " << id << ", skipping!" << std::endl;
                continue;
            }

            if (views[id] == NULL || !views[id]->is_camera_valid())
                continue;

            /* Setup MVS. */
            mvs::Settings settings(conf.mvs);
            settings.refViewNr = id;
            if (conf.max_pixels > 0)
                settings.scale = get_scale_from_max_pixels(scene, conf, settings);

            std::string embedding_name = "depth-L"
                + util::string::get(settings.scale);
            if (!conf.force_recon && views[id]->has_embedding(embedding_name))
                continue;

            try
            {
                reconstruct(scene, settings);
                views[id]->save_mve_file();
            }
            catch (std::exception &err)
            {
                std::cerr << err.what() << std::endl;
            }
        }
    }

    if (conf.progress_style == PROGRESS_FANCY)
    {
        fancyProgressPrinter.stop();
        fancyProgressPrinter.pt_join();
    }

    std::cout << "Reconstruction took "
        << timer.get_elapsed() << "ms." << std::endl;

    /* Save scene */
    std::cout << "Saving views back to disc..." << std::endl;
    scene->save_views();

    return 0;
}
