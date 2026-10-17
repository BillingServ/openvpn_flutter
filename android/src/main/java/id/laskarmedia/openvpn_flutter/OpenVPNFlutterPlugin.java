package id.laskarmedia.openvpn_flutter;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.VpnService;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import androidx.annotation.NonNull;

import java.util.ArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import io.flutter.embedding.engine.plugins.FlutterPlugin;
import io.flutter.embedding.engine.plugins.activity.ActivityAware;
import io.flutter.embedding.engine.plugins.activity.ActivityPluginBinding;
import io.flutter.plugin.common.EventChannel;
import io.flutter.plugin.common.MethodChannel;
import io.flutter.plugin.common.PluginRegistry;

/**
 * OpenvpnFlutterPlugin
 *
 * Per-engine adapter. Connection state lives in {@link OpenVpnRuntime}, which is process-wide.
 */
public class OpenVPNFlutterPlugin implements FlutterPlugin, ActivityAware,
        PluginRegistry.ActivityResultListener {

    private static final String TAG = "OpenVPNFlutterPlugin";
    private static final int PERMISSION_REQUEST_CODE = 24;

    private MethodChannel vpnControlMethod;
    private EventChannel vpnStageEvent;
    private OpenVpnRuntime.StageListener stageListener;

    private static final String EVENT_CHANNEL_VPN_STAGE = "id.laskarmedia.openvpn_flutter/vpnstage";
    private static final String METHOD_CHANNEL_VPN_CONTROL = "id.laskarmedia.openvpn_flutter/vpncontrol";

    private static final ExecutorService worker = Executors.newCachedThreadPool(runnable -> {
        Thread thread = new Thread(runnable, "openvpn-flutter");
        thread.setDaemon(true);
        return thread;
    });

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private ActivityPluginBinding activityBinding;

    Context mContext;

    /**
     * Kept for existing callers that forward the VPN consent result themselves. The plugin
     * also receives the result through its own activity result listener; whichever call
     * arrives first resolves the pending connect.
     */
    public static void connectWhileGranted(boolean granted) {
        OpenVpnRuntime.onPermissionResult(granted);
    }

    @Override
    public void onAttachedToEngine(@NonNull FlutterPluginBinding binding) {
        mContext = binding.getApplicationContext();
        OpenVpnRuntime.init(mContext);
        vpnStageEvent = new EventChannel(binding.getBinaryMessenger(), EVENT_CHANNEL_VPN_STAGE);
        vpnControlMethod = new MethodChannel(binding.getBinaryMessenger(), METHOD_CHANNEL_VPN_CONTROL);

        vpnStageEvent.setStreamHandler(new EventChannel.StreamHandler() {
            @Override
            public void onListen(Object arguments, EventChannel.EventSink events) {
                clearStageListener();
                if (events == null) return;
                stageListener = events::success;
                OpenVpnRuntime.addListener(stageListener);
                // Replay the current stage for the new listener.
                events.success(OpenVpnRuntime.getStage());
            }

            @Override
            public void onCancel(Object arguments) {
                clearStageListener();
            }
        });

        vpnControlMethod.setMethodCallHandler((call, result) -> {

            switch (call.method) {
                case "status":
                    result.success(OpenVpnRuntime.statusJson());
                    break;
                case "initialize":
                    OpenVpnRuntime.init(mContext);
                    result.success(OpenVpnRuntime.getStage());
                    break;
                case "disconnect":
                    worker.execute(() -> {
                        boolean stopped = OpenVpnRuntime.stop(OpenVpnRuntime.DEFAULT_STOP_TIMEOUT_MS);
                        mainHandler.post(() -> result.success(stopped));
                    });
                    break;
                case "connect": {
                    final String config = call.argument("config");
                    final String name = call.argument("name");
                    final String username = call.argument("username");
                    final String password = call.argument("password");
                    final ArrayList<String> bypassPackages = call.argument("bypass_packages");

                    if (config == null) {
                        result.error("-2", "OpenVPN Config is required", "");
                        return;
                    }

                    final Activity activity = getActivity();
                    final Intent permission;
                    try {
                        permission = VpnService.prepare(activity != null ? activity : mContext);
                    } catch (Exception e) {
                        result.error("-3", "Could not check VPN permission: " + e.getMessage(), "");
                        return;
                    }
                    if (permission != null) {
                        if (activity == null) {
                            result.error("-3", "VPN permission required but no activity is attached", "");
                            return;
                        }
                        OpenVpnRuntime.setPendingStart(config, name, username, password, bypassPackages,
                                (started, error) -> {
                                    if (error != null && !started) {
                                        result.error("-4", error, "");
                                    } else {
                                        result.success(started);
                                    }
                                });
                        try {
                            activity.startActivityForResult(permission, PERMISSION_REQUEST_CODE);
                        } catch (Exception e) {
                            OpenVpnRuntime.cancelPendingStart("Could not show VPN permission dialog: " + e.getMessage());
                        }
                        return;
                    }
                    final long generation = OpenVpnRuntime.startGeneration();
                    worker.execute(() -> {
                        try {
                            OpenVpnRuntime.start(mContext, config, name, username, password, bypassPackages, generation);
                            mainHandler.post(() -> result.success(true));
                        } catch (Throwable t) {
                            Log.e(TAG, "connect failed: " + t);
                            final String message = t.getMessage() != null ? t.getMessage() : t.toString();
                            mainHandler.post(() -> result.error("-4", message, ""));
                        }
                    });
                    break;
                }
                case "stage":
                    result.success(OpenVpnRuntime.getStage());
                    break;
                case "request_permission": {
                    final Activity activity = getActivity();
                    final Intent request;
                    try {
                        request = VpnService.prepare(activity != null ? activity : mContext);
                    } catch (Exception e) {
                        result.success(false);
                        return;
                    }
                    if (request != null) {
                        if (activity != null) {
                            try {
                                activity.startActivityForResult(request, PERMISSION_REQUEST_CODE);
                            } catch (Exception e) {
                                Log.e(TAG, "Could not show VPN permission dialog: " + e.getMessage());
                            }
                        }
                        result.success(false);
                        return;
                    }
                    result.success(true);
                    break;
                }

                default:
                    result.notImplemented();
            }
        });
    }

    private Activity getActivity() {
        return activityBinding != null ? activityBinding.getActivity() : null;
    }

    private void clearStageListener() {
        if (stageListener != null) {
            OpenVpnRuntime.removeListener(stageListener);
            stageListener = null;
        }
    }

    @Override
    public void onDetachedFromEngine(@NonNull FlutterPluginBinding binding) {
        vpnStageEvent.setStreamHandler(null);
        vpnControlMethod.setMethodCallHandler(null);
        clearStageListener();
        OpenVpnRuntime.cancelPendingStart("Flutter engine detached");
    }

    @Override
    public boolean onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != PERMISSION_REQUEST_CODE) return false;
        OpenVpnRuntime.onPermissionResult(resultCode == Activity.RESULT_OK);
        return true;
    }

    @Override
    public void onAttachedToActivity(@NonNull ActivityPluginBinding binding) {
        attachActivity(binding);
    }

    @Override
    public void onDetachedFromActivityForConfigChanges() {
        detachActivity();
    }

    @Override
    public void onReattachedToActivityForConfigChanges(@NonNull ActivityPluginBinding binding) {
        attachActivity(binding);
    }

    @Override
    public void onDetachedFromActivity() {
        detachActivity();
    }

    private void attachActivity(ActivityPluginBinding binding) {
        if (activityBinding != null) activityBinding.removeActivityResultListener(this);
        activityBinding = binding;
        binding.addActivityResultListener(this);
    }

    private void detachActivity() {
        if (activityBinding != null) activityBinding.removeActivityResultListener(this);
        activityBinding = null;
    }
}
