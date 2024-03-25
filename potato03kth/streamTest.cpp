#include <librealsense2/rs.hpp> // Include RealSense Cross Platform API
#include "example.hpp"          // Include for QoL

// function that below flows as a "try and catch" sentention
// every 1 loops excute if there arn't error catched
// ? we don't know what argc is.
int main(int argc, char *argv[])
try
{
    rs2::log_to_console(RS2_LOG_SEVERITY_ERROR // send pre-definated type of error to console
    );
    // window from OpenGL
    window app(1920, 1080, "Stream test");

    // -----<<declareration>>-----

    // declare depth map colorizer
    rs2::colorizer color_mapping;
    // declare for showing streaming rates.
    rs2::rates_printer print;
    rs2::pipeline p;

    // ---------------------------

    p.start();

    while (app) // mathod of OpenGL we dont know about OpenGL
    {
        rs2::frameset d = p.wait_for_frames()
                              .apply_filter(print)          // ? Print fps
                              .apply_filter(color_mapping); // colorizer

        // mathod of OpenGL we dont know about OpenGL
        // TODO we need to convert this to OpenCV format
        app.show(d);
    }
}
catch (const rs2::error &e)
{
    std::cerr << '\n'
              << "-----"
              << "error at RealSense!" << e.what()
              << '\n'
              << "-----";
    return EXIT_FAILURE; // this value declared by compiler
}
catch (const std::exception &e)
{
    std::cerr << e.what() << '\n';
    return EXIT_FAILURE; // this value declared by compiler
}
