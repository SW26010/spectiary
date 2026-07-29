#include "app/on_demand_frame_capture.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(
    bool condition,
    std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void TestExperimentalEnablementIsExactAndDefaultOff()
{
    const specforge::OnDemandFrameCaptureConfiguration
        missing =
            specforge::ResolveOnDemandFrameCapture(
                std::nullopt);
    Require(
        !missing.enabled && missing.recognized &&
            missing.requested.empty(),
        "missing configuration should keep experimental capture disabled");

    const specforge::OnDemandFrameCaptureConfiguration
        enabled =
            specforge::ResolveOnDemandFrameCapture("1");
    Require(
        enabled.enabled && enabled.recognized &&
            enabled.requested == "1",
        "the exact experimental value should enable capture");

    const specforge::OnDemandFrameCaptureConfiguration
        unknown =
            specforge::ResolveOnDemandFrameCapture(
                "true");
    Require(
        !unknown.enabled && !unknown.recognized &&
            unknown.requested == "true",
        "unknown values should remain disabled and observable");
}

void TestRequestTargetsTheFollowingFrame()
{
    specforge::OnDemandFrameCapture capture(
        specforge::ResolveOnDemandFrameCapture(
            "1"));

    Require(
        capture.Request(41, true) ==
            specforge::
                OnDemandFrameCaptureRequestOutcome::
                    Accepted,
        "a renderable enabled window should accept a request");
    Require(
        capture.pending(),
        "an accepted request should remain pending");
    Require(
        capture.status() ==
            specforge::OnDemandFrameCaptureStatus::
                Pending,
        "an accepted request should expose semantic pending status");
    Require(
        !capture.ShouldCapture(41),
        "the frame in which the request was accepted should not be captured");
    Require(
        capture.ShouldCapture(42),
        "the first following frame should satisfy the request");

    const std::filesystem::path output =
        L"C:\\captures\\frame-42.png";
    capture.Complete(output);
    Require(
        !capture.pending() &&
            capture.last_output_path() ==
                output &&
            capture.status() ==
                specforge::
                    OnDemandFrameCaptureStatus::
                        Captured,
        "completion should publish only the newly captured output");
}

void TestDuplicateRequestDoesNotMoveTheTarget()
{
    specforge::OnDemandFrameCapture capture(
        specforge::ResolveOnDemandFrameCapture(
            "1"));

    Require(
        capture.Request(10, true) ==
            specforge::
                OnDemandFrameCaptureRequestOutcome::
                    Accepted,
        "the initial request should be accepted");
    Require(
        capture.Request(20, true) ==
            specforge::
                OnDemandFrameCaptureRequestOutcome::
                    AlreadyPending,
        "a second request should not replace a pending request");
    Require(
        capture.ShouldCapture(11),
        "a duplicate request must not postpone the original next-frame target");
}

void TestNonRenderableWindowRejectsAndCancels()
{
    specforge::OnDemandFrameCapture capture(
        specforge::ResolveOnDemandFrameCapture(
            "1"));
    Require(
        capture.Request(7, false) ==
            specforge::
                OnDemandFrameCaptureRequestOutcome::
                    WindowNotRenderable,
        "a minimized or hidden window should reject capture immediately");
    Require(
        !capture.pending() &&
            capture.status_message().find(
                "minimized or hidden") !=
                std::string_view::npos &&
            capture.status() ==
                specforge::
                    OnDemandFrameCaptureStatus::
                        WindowUnavailable,
        "the rejection should have explicit unavailable semantics");

    Require(
        capture.Request(8, true) ==
            specforge::
                OnDemandFrameCaptureRequestOutcome::
                    Accepted,
        "a later renderable request should be accepted");
    capture.ObserveWindowRenderable(false);
    Require(
        !capture.pending(),
        "becoming non-renderable should cancel rather than defer the request");
    capture.ObserveWindowRenderable(true);
    Require(
        !capture.ShouldCapture(100),
        "restoring the window must not resurrect a canceled request");
}

void TestTypedFailureStatusRetainsTechnicalDetail()
{
    specforge::OnDemandFrameCapture capture(
        specforge::ResolveOnDemandFrameCapture("1"));

    capture.FailCapture(
        "Direct3D/WIC capture",
        "0x80004005");
    Require(
        capture.status() ==
                specforge::
                    OnDemandFrameCaptureStatus::
                        FailedCapture &&
            capture.status_operation() ==
                "Direct3D/WIC capture" &&
            capture.status_result() ==
                "0x80004005",
        "capture failure should expose semantic state and technical detail");

    capture.FailPreparingOutputDirectory();
    Require(
        capture.status() ==
            specforge::OnDemandFrameCaptureStatus::
                FailedPreparingOutputDirectory,
        "directory preparation failure should remain distinguishable");
}

void TestDisabledCaptureCannotBecomePending()
{
    specforge::OnDemandFrameCapture capture;
    Require(
        capture.Request(1, true) ==
            specforge::
                OnDemandFrameCaptureRequestOutcome::
                    Disabled,
        "disabled capture should reject requests");
    Require(
        !capture.pending() &&
            !capture.ShouldCapture(2),
        "disabled capture should never schedule capture work");
}

}  // namespace

int main()
{
    TestExperimentalEnablementIsExactAndDefaultOff();
    TestRequestTargetsTheFollowingFrame();
    TestDuplicateRequestDoesNotMoveTheTarget();
    TestNonRenderableWindowRejectsAndCancels();
    TestDisabledCaptureCannotBecomePending();
    TestTypedFailureStatusRetainsTechnicalDetail();
    return 0;
}
