/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <linux/media-bus-format.h>
#include <linux/media.h>
#include <linux/videodev2.h>

#include <algorithm>

#include <gtest/gtest.h>

#include "provider/Discovery.h"
#include "provider/MediaPipeline.h"
#include "session/PipelineController.h"
#include "tests/FakeMedia.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

constexpr uint32_t kEnabled = MEDIA_LNK_FL_ENABLED;
constexpr uint32_t kImmutable = MEDIA_LNK_FL_IMMUTABLE;

FormatDescription Pixel(uint32_t fourcc) {
    FormatDescription format;
    format.fourcc = fourcc;
    return format;
}

// Like the vimc test driver: a Bayer sensor feeding a raw capture node and,
// through a debayer and a scaler, an RGB capture node.
class VimcGraph : public ::testing::Test {
  protected:
    void SetUp() override {
        sensor_ = graph_.AddEntity("Sensor A", MEDIA_ENT_F_CAM_SENSOR, "/dev/v4l-subdev0", 0, 1);
        debayer_ = graph_.AddEntity("Debayer A", MEDIA_ENT_F_PROC_VIDEO_PIXEL_ENC_CONV,
                                    "/dev/v4l-subdev2", 1, 1);
        scaler_ =
                graph_.AddEntity("Scaler", MEDIA_ENT_F_PROC_VIDEO_SCALER, "/dev/v4l-subdev4", 1, 1);
        raw_ = graph_.AddEntity("Raw Capture 0", MEDIA_ENT_F_IO_V4L, "/dev/video0", 1, 0);
        rgb_ = graph_.AddEntity("RGB/YUV Capture", MEDIA_ENT_F_IO_V4L, "/dev/video2", 1, 0);
        graph_.AddLink(sensor_, 0, debayer_, 0, kEnabled | kImmutable);
        graph_.AddLink(debayer_, 1, scaler_, 0, kEnabled);
        graph_.AddLink(scaler_, 1, rgb_, 0, kEnabled | kImmutable);
        graph_.AddLink(sensor_, 0, raw_, 0, kEnabled | kImmutable);

        auto& sensor = graph_.SubDevice("/dev/v4l-subdev0");
        sensor.codes[0] = {MEDIA_BUS_FMT_SGRBG8_1X8};
        sensor.sizes[{0, MEDIA_BUS_FMT_SGRBG8_1X8}] = {{640, 480, {}}, {1920, 1080, {{1, 30}}}};
        sensor.controls[V4L2_CID_CAMERA_ORIENTATION] = V4L2_CAMERA_ORIENTATION_FRONT;
        graph_.SubDevice("/dev/v4l-subdev2").codes[1] = {MEDIA_BUS_FMT_RGB888_1X24};
        graph_.SetVideoFormats("/dev/video2", MEDIA_BUS_FMT_RGB888_1X24,
                               {Pixel(V4L2_PIX_FMT_RGB24), Pixel(V4L2_PIX_FMT_BGR24)});
        graph_.SetVideoFormats("/dev/video0", MEDIA_BUS_FMT_SGRBG8_1X8,
                               {Pixel(V4L2_PIX_FMT_SGRBG8)});
    }

    FakeMediaGraph graph_;
    uint32_t sensor_, debayer_, scaler_, raw_, rgb_;
};

TEST_F(VimcGraph, PicksTheProcessedPath) {
    auto open = graph_.Openers();
    auto media = open.media("/dev/media0");
    ASSERT_TRUE(media.ok());
    const auto cameras = DiscoverMediaCameras(media->get(), open);
    ASSERT_EQ(cameras.size(), 1u);
    const auto& camera = cameras[0];
    ASSERT_NE(camera.pipeline, nullptr);
    EXPECT_EQ(camera.pipeline->video_node, "/dev/video2");
    EXPECT_EQ(camera.pipeline->sensor_subdev, "/dev/v4l-subdev0");
    ASSERT_EQ(camera.pipeline->hops.size(), 3u);
    EXPECT_TRUE(camera.pipeline->hops[0].converter);
    EXPECT_FALSE(camera.pipeline->hops[1].converter);
    EXPECT_TRUE(camera.pipeline->hops[2].sink_subdev.empty());

    ASSERT_EQ(camera.formats.size(), 2u);
    EXPECT_EQ(camera.formats[0].fourcc, V4L2_PIX_FMT_RGB24);
    ASSERT_EQ(camera.formats[0].sizes.size(), 2u);
    // Sizes without intervals get a default one.
    for (const auto& size : camera.formats[0].sizes) EXPECT_FALSE(size.intervals.empty());

    const auto* format = camera.pipeline->FindFormat(V4L2_PIX_FMT_BGR24);
    ASSERT_NE(format, nullptr);
    EXPECT_EQ(format->sensor_code, uint32_t{MEDIA_BUS_FMT_SGRBG8_1X8});
    EXPECT_EQ(format->output_code, uint32_t{MEDIA_BUS_FMT_RGB888_1X24});
}

TEST_F(VimcGraph, ConfiguresTheStages) {
    auto open = graph_.Openers();
    auto media = open.media("/dev/media0");
    ASSERT_TRUE(media.ok());
    auto cameras = DiscoverMediaCameras(media->get(), open);
    ASSERT_EQ(cameras.size(), 1u);
    ASSERT_NE(cameras[0].pipeline, nullptr);

    auto controller = PipelineController::Open(cameras[0].pipeline, open);
    ASSERT_TRUE(controller.ok());
    auto size = (*controller)->Configure(V4L2_PIX_FMT_RGB24, {1920, 1080});
    ASSERT_TRUE(size.ok()) << size.error().message();
    EXPECT_EQ(*size, (Size{1920, 1080}));

    // All links were enabled already.
    EXPECT_TRUE(graph_.link_changes().empty());
    auto& sensor = graph_.SubDevice("/dev/v4l-subdev0");
    EXPECT_EQ(sensor.formats[0].code, uint32_t{MEDIA_BUS_FMT_SGRBG8_1X8});
    auto& debayer = graph_.SubDevice("/dev/v4l-subdev2");
    EXPECT_EQ(debayer.formats[0].code, uint32_t{MEDIA_BUS_FMT_SGRBG8_1X8});
    EXPECT_EQ(debayer.formats[1].code, uint32_t{MEDIA_BUS_FMT_RGB888_1X24});
    auto& scaler = graph_.SubDevice("/dev/v4l-subdev4");
    EXPECT_EQ(scaler.formats[0].code, uint32_t{MEDIA_BUS_FMT_RGB888_1X24});
    EXPECT_EQ(scaler.formats[1].width, 1920u);

    // Not a format of the pipeline.
    EXPECT_FALSE((*controller)->Configure(V4L2_PIX_FMT_YUYV, {1920, 1080}).ok());
}

TEST_F(VimcGraph, BecomesAnInternalCamera) {
    auto open = graph_.Openers();
    auto media = open.media("/dev/media0");
    ASSERT_TRUE(media.ok());
    const auto candidates = ProbeMediaDevice(Properties{}, media->get(), open);
    ASSERT_EQ(candidates.size(), 1u);
    const auto& camera = candidates[0];
    EXPECT_EQ(camera.key, "platform:fake|Sensor A");
    EXPECT_EQ(camera.info.path, "/dev/video2");
    EXPECT_TRUE(camera.internal);
    EXPECT_EQ(camera.facing, Facing::kFront);
    EXPECT_EQ(camera.facing_source, "firmware");
    EXPECT_NE(camera.pipeline, nullptr);
    EXPECT_EQ(camera.selectors, (std::vector<std::string>{"Sensor_A", "Sensor"}));
}

// Like Qualcomm CAMSS: sensor -> CSIPHY -> CSID -> ISPIF -> VFE RDI -> video
// node, with links that have to be enabled, and alternatives.
class CamssGraph : public ::testing::Test {
  protected:
    void Build(uint32_t sensor_code) {
        sensor_ = graph_.AddEntity("ov5675 2-0036", MEDIA_ENT_F_CAM_SENSOR, "/dev/v4l-subdev10", 0,
                                   1);
        csiphy_ = graph_.AddEntity("msm_csiphy0", MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER,
                                   "/dev/v4l-subdev0", 1, 1);
        csid0_ = graph_.AddEntity("msm_csid0", MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER,
                                  "/dev/v4l-subdev1", 1, 1);
        csid1_ = graph_.AddEntity("msm_csid1", MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER,
                                  "/dev/v4l-subdev2", 1, 1);
        ispif_ = graph_.AddEntity("msm_ispif0", MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER,
                                  "/dev/v4l-subdev3", 1, 1);
        rdi_ = graph_.AddEntity("msm_vfe0_rdi0", MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER,
                                "/dev/v4l-subdev4", 1, 1);
        video_ = graph_.AddEntity("msm_vfe0_video0", MEDIA_ENT_F_IO_V4L, "/dev/video0", 1, 0);

        graph_.AddLink(sensor_, 0, csiphy_, 0, kEnabled | kImmutable);
        csiphy_csid0_ = graph_.AddLink(csiphy_, 1, csid0_, 0, 0);
        graph_.AddLink(csiphy_, 1, csid1_, 0, 0);
        csid0_ispif_ = graph_.AddLink(csid0_, 1, ispif_, 0, 0);
        // Another camera left this one enabled.
        csid1_ispif_ = graph_.AddLink(csid1_, 1, ispif_, 0, kEnabled);
        ispif_rdi_ = graph_.AddLink(ispif_, 1, rdi_, 0, 0);
        graph_.AddLink(rdi_, 1, video_, 0, kEnabled | kImmutable);

        auto& sensor = graph_.SubDevice("/dev/v4l-subdev10");
        sensor.codes[0] = {sensor_code};
        sensor.sizes[{0, sensor_code}] = {{2592, 1944, {{1, 30}}}};
        graph_.SetVideoFormats("/dev/video0", MEDIA_BUS_FMT_SGRBG10_1X10,
                               {Pixel(V4L2_PIX_FMT_SGRBG10P), Pixel(V4L2_PIX_FMT_SGRBG10)});
        graph_.SetVideoFormats("/dev/video0", MEDIA_BUS_FMT_YUYV8_2X8, {Pixel(V4L2_PIX_FMT_YUYV)});
    }

    FakeMediaGraph graph_;
    uint32_t sensor_, csiphy_, csid0_, csid1_, ispif_, rdi_, video_;
    uint32_t csiphy_csid0_, csid0_ispif_, csid1_ispif_, ispif_rdi_;
};

TEST_F(CamssGraph, RawSensorNeedsIsp) {
    Build(MEDIA_BUS_FMT_SGRBG10_1X10);
    auto open = graph_.Openers();
    auto media = open.media("/dev/media0");
    ASSERT_TRUE(media.ok());
    const auto cameras = DiscoverMediaCameras(media->get(), open);
    ASSERT_EQ(cameras.size(), 1u);
    EXPECT_EQ(cameras[0].pipeline, nullptr);
    EXPECT_TRUE(cameras[0].raw_only);
    EXPECT_TRUE(ProbeMediaDevice(Properties{}, media->get(), open).empty());
}

TEST_F(CamssGraph, YuvSensor) {
    Build(MEDIA_BUS_FMT_YUYV8_2X8);
    auto open = graph_.Openers();
    auto media = open.media("/dev/media0");
    ASSERT_TRUE(media.ok());
    auto cameras = DiscoverMediaCameras(media->get(), open);
    ASSERT_EQ(cameras.size(), 1u);
    ASSERT_NE(cameras[0].pipeline, nullptr);
    const auto& pipeline = *cameras[0].pipeline;
    ASSERT_EQ(cameras[0].formats.size(), 1u);
    EXPECT_EQ(cameras[0].formats[0].fourcc, V4L2_PIX_FMT_YUYV);
    ASSERT_EQ(pipeline.hops.size(), 5u);
    // Through CSID 1, whose link to the ISPIF is enabled already.
    EXPECT_EQ(pipeline.hops[1].sink_entity, "msm_csid1");

    auto controller = PipelineController::Open(cameras[0].pipeline, open);
    ASSERT_TRUE(controller.ok());
    auto size = (*controller)->Configure(V4L2_PIX_FMT_YUYV, {2592, 1944});
    ASSERT_TRUE(size.ok()) << size.error().message();
    EXPECT_EQ(*size, (Size{2592, 1944}));
    // The disabled links on the path got enabled.
    const auto& changes = graph_.link_changes();
    EXPECT_NE(std::find(changes.begin(), changes.end(), std::make_pair(ispif_rdi_, true)),
              changes.end());
    EXPECT_EQ(std::find(changes.begin(), changes.end(), std::make_pair(csid1_ispif_, false)),
              changes.end());
}

TEST_F(CamssGraph, LinkedFlash) {
    Build(MEDIA_BUS_FMT_YUYV8_2X8);
    const uint32_t flash =
            graph_.AddEntity("qcom-flash 1-d300", MEDIA_ENT_F_FLASH, "/dev/v4l-subdev20", 0, 0);
    graph_.topology().links.push_back(
            {.id = 999,
             .source = sensor_,
             .sink = flash,
             .flags = MEDIA_LNK_FL_ANCILLARY_LINK | MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE});
    auto open = graph_.Openers();
    auto media = open.media("/dev/media0");
    ASSERT_TRUE(media.ok());
    auto cameras = DiscoverMediaCameras(media->get(), open);
    ASSERT_EQ(cameras.size(), 1u);
    ASSERT_NE(cameras[0].pipeline, nullptr);
    EXPECT_EQ(cameras[0].pipeline->flash_subdevs, std::vector<std::string>{"/dev/v4l-subdev20"});
}

TEST_F(CamssGraph, DisablesCompetingLinks) {
    Build(MEDIA_BUS_FMT_YUYV8_2X8);
    auto open = graph_.Openers();
    auto media = open.media("/dev/media0");
    ASSERT_TRUE(media.ok());
    auto cameras = DiscoverMediaCameras(media->get(), open);
    ASSERT_NE(cameras[0].pipeline, nullptr);

    // Force the path through CSID 0, as if CSID 1 were someone else's.
    auto pipeline = std::make_shared<MediaPipeline>(*cameras[0].pipeline);
    for (const auto& link : graph_.topology().links) {
        if (link.id == csiphy_csid0_) pipeline->hops[1].link = link;
        if (link.id == csid0_ispif_) pipeline->hops[2].link = link;
    }
    pipeline->hops[1].sink_entity = "msm_csid0";
    pipeline->hops[1].sink_subdev = "/dev/v4l-subdev1";

    auto controller = PipelineController::Open(pipeline, open);
    ASSERT_TRUE(controller.ok());
    ASSERT_TRUE((*controller)->Configure(V4L2_PIX_FMT_YUYV, {2592, 1944}).ok());
    const auto& changes = graph_.link_changes();
    // CSID 1 no longer feeds the ISPIF; CSID 0 does.
    EXPECT_NE(std::find(changes.begin(), changes.end(), std::make_pair(csid1_ispif_, false)),
              changes.end());
    EXPECT_NE(std::find(changes.begin(), changes.end(), std::make_pair(csid0_ispif_, true)),
              changes.end());
}

TEST(PixelFormatsForMbusCodeTest, Mapping) {
    EXPECT_EQ(PixelFormatsForMbusCode(MEDIA_BUS_FMT_YUYV8_2X8),
              (std::vector<uint32_t>{V4L2_PIX_FMT_YUYV}));
    EXPECT_EQ(PixelFormatsForMbusCode(MEDIA_BUS_FMT_UYVY8_1X16),
              (std::vector<uint32_t>{V4L2_PIX_FMT_UYVY}));
    EXPECT_FALSE(PixelFormatsForMbusCode(MEDIA_BUS_FMT_RGB888_1X24).empty());
    EXPECT_TRUE(PixelFormatsForMbusCode(MEDIA_BUS_FMT_SGRBG10_1X10).empty());
    EXPECT_TRUE(IsBayerMbusCode(MEDIA_BUS_FMT_SGRBG10_1X10));
    EXPECT_FALSE(IsBayerMbusCode(MEDIA_BUS_FMT_YUYV8_2X8));
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
