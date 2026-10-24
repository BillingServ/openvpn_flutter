package id.laskarmedia.openvpn_flutter;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;
import android.os.SystemClock;
import android.util.Log;

import java.io.IOException;
import java.io.StringReader;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;
import java.util.concurrent.CopyOnWriteArraySet;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.ReentrantLock;

import de.blinkt.openvpn.R;
import de.blinkt.openvpn.VpnProfile;
import de.blinkt.openvpn.core.ConfigParser;
import de.blinkt.openvpn.core.ConnectionStatus;
import de.blinkt.openvpn.core.OpenVPNService;
import de.blinkt.openvpn.core.OpenVPNThread;
import de.blinkt.openvpn.core.ProfileManager;
import de.blinkt.openvpn.core.VPNLaunchHelper;
import de.blinkt.openvpn.core.VpnStatus;

/**
 * Process-wide view of the OpenVPN connection.
 *
 * State comes from the library's own in-process VpnStatus listeners, so it survives Flutter
 * engine recreation and does not depend on an Activity. Blocking calls ({@link #stop(long)},
 * {@link #start}) must not run on the main thread.
 */
public final class OpenVpnRuntime {
    private static final String TAG = "OpenVpnRuntime";

    public static final String STATE_UP = "up";
    public static final String STATE_CONNECTING = "connecting";
    public static final String STATE_DOWN = "down";

    public static final long DEFAULT_STOP_TIMEOUT_MS = 8000L;

    // A start that has not produced any library state yet counts as connecting for this long.
    private static final long START_GRACE_MS = 20000L;

    /** Receives stage strings on the main thread. */
    public interface StageListener {
        void onStageChanged(String stage);
    }

    /** Cumulative byte counters of the current session. */
    public static final class Statistics {
        public final long rxBytes;
        public final long txBytes;
        public final Long connectedSinceEpochMs;

        Statistics(long rxBytes, long txBytes, Long connectedSinceEpochMs) {
            this.rxBytes = rxBytes;
            this.txBytes = txBytes;
            this.connectedSinceEpochMs = connectedSinceEpochMs;
        }
    }

    /** Called on the main thread once a start waiting for VPN consent is resolved. */
    public interface PendingStartCallback {
        void onResolved(boolean started, String error);
    }

    private static final class PendingStart {
        final String config;
        final String name;
        final String username;
        final String password;
        final List<String> bypassPackages;
        final PendingStartCallback callback;
        final long generation = startGeneration();

        PendingStart(String config, String name, String username, String password,
                     List<String> bypassPackages, PendingStartCallback callback) {
            this.config = config;
            this.name = name;
            this.username = username;
            this.password = password;
            this.bypassPackages = bypassPackages;
            this.callback = callback;
        }
    }

    private static final Handler mainHandler = new Handler(Looper.getMainLooper());
    private static final CopyOnWriteArraySet<StageListener> listeners = new CopyOnWriteArraySet<>();

    private static volatile Context appContext;
    private static volatile String rawState = "NOPROCESS";
    private static volatile ConnectionStatus level = ConnectionStatus.LEVEL_NOTCONNECTED;
    private static volatile String stage = "disconnected";
    private static volatile long rxBytes = 0;
    private static volatile long txBytes = 0;
    private static volatile long connectedSinceEpochMs = 0;
    private static volatile long startRequestedAt = 0;
    private static volatile PendingStart pendingStart;
    private static final AtomicLong stopGeneration = new AtomicLong();
    private static final ReentrantLock operationLock = new ReentrantLock();

    /**
     * Runs on the starting thread before a connection is started. The app uses it to stop its
     * other protocols first, since all of them share the app's single VPN slot.
     */
    public static volatile Runnable beforeStart;

    private static final VpnStatus.StateListener stateListener = new VpnStatus.StateListener() {
        @Override
        public void updateState(String state, String logmessage, int localizedResId,
                                ConnectionStatus newLevel, Intent intent) {
            onLibraryState(state, newLevel);
        }

        @Override
        public void setConnectedVPN(String uuid) {
        }
    };

    private static final VpnStatus.ByteCountListener byteCountListener =
            (in, out, diffIn, diffOut) -> {
                rxBytes = in;
                txBytes = out;
            };

    private OpenVpnRuntime() {
    }

    public static synchronized void init(Context context) {
        if (appContext != null || context == null) return;
        appContext = context.getApplicationContext();
        try {
            VpnStatus.initLogCache(appContext.getCacheDir());
        } catch (Throwable t) {
            Log.w(TAG, "initLogCache failed: " + t.getMessage());
        }
        // Both calls replay the latest library state to the new listener.
        VpnStatus.addStateListener(stateListener);
        VpnStatus.addByteCountListener(byteCountListener);
    }

    public static void addListener(StageListener listener) {
        listeners.add(listener);
    }

    public static void removeListener(StageListener listener) {
        listeners.remove(listener);
    }

    /** The stage string the Dart side understands. */
    public static String getStage() {
        if (STATE_CONNECTING.equals(getState()) && "disconnected".equals(stage)) {
            return "connecting";
        }
        return stage;
    }

    /** "up", "connecting" or "down". */
    public static String getState() {
        ConnectionStatus current = level;
        if (current == ConnectionStatus.LEVEL_CONNECTED) return STATE_UP;
        if (pendingStart != null) return STATE_CONNECTING;
        boolean processGone = current == ConnectionStatus.LEVEL_NOTCONNECTED
                && !"EXITING".equalsIgnoreCase(rawState);
        if (!processGone) return STATE_CONNECTING;
        long requested = startRequestedAt;
        if (requested != 0 && SystemClock.elapsedRealtime() - requested < START_GRACE_MS) {
            return STATE_CONNECTING;
        }
        return STATE_DOWN;
    }

    public static boolean isUp() {
        return STATE_UP.equals(getState());
    }

    public static Statistics getStatistics() {
        if (!isUp()) return null;
        long since = connectedSinceEpochMs;
        return new Statistics(rxBytes, txBytes, since > 0 ? since : null);
    }

    /** Status JSON in the format the Dart side parses. */
    public static String statusJson() {
        long since = connectedSinceEpochMs;
        String connectedOn = "";
        String duration = "00:00:00";
        if (isUp() && since > 0) {
            SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.US);
            format.setTimeZone(TimeZone.getTimeZone("UTC"));
            connectedOn = format.format(new Date(since));
            long seconds = Math.max(0, (System.currentTimeMillis() - since) / 1000);
            duration = String.format(Locale.US, "%02d:%02d:%02d",
                    seconds / 3600, (seconds % 3600) / 60, seconds % 60);
        }
        try {
            org.json.JSONObject json = new org.json.JSONObject();
            json.put("connected_on", connectedOn);
            json.put("duration", duration);
            json.put("last_packet_receive", "0");
            json.put("byte_in", String.valueOf(isUp() ? rxBytes : 0));
            json.put("byte_out", String.valueOf(isUp() ? txBytes : 0));
            return json.toString();
        } catch (org.json.JSONException e) {
            return "{}";
        }
    }

    /** Starts a connection. VPN consent must already be granted. Off the main thread. */
    public static long startGeneration() {
        return stopGeneration.get();
    }

    public static void start(Context context, String config, String name, String username,
                             String password, List<String> bypassPackages) throws RemoteException {
        start(context, config, name, username, password, bypassPackages, startGeneration());
    }

    public static void start(Context context, String config, String name, String username,
                             String password, List<String> bypassPackages, long generation) throws RemoteException {
        if (generation != stopGeneration.get()) {
            throw new IllegalStateException("Start cancelled by a stop request");
        }
        init(context);
        Runnable hook = beforeStart;
        if (hook != null) {
            try {
                hook.run();
            } catch (Throwable t) {
                Log.w(TAG, "beforeStart hook failed: " + t.getMessage());
            }
        }
        operationLock.lock();
        try {
            if (generation != stopGeneration.get()) {
                throw new IllegalStateException("Start cancelled by a stop request");
            }
            rxBytes = 0;
            txBytes = 0;
            connectedSinceEpochMs = 0;
            startRequestedAt = SystemClock.elapsedRealtime();
            updateStage("prepare");
            try {
                startVpn(appContext, config, name, username, password, bypassPackages);
                if (generation != stopGeneration.get()) {
                    killProcess();
                    requestStopViaService(2000L);
                    throw new IllegalStateException("Start cancelled by a stop request");
                }
            } catch (RemoteException | RuntimeException e) {
                startRequestedAt = 0;
                updateStage(mapStage(rawState, level));
                throw e;
            }
        } finally {
            operationLock.unlock();
        }
    }

    /**
     * OpenVpnApi.startVpn, except that bypass packages are only excluded from the tunnel.
     * The library also turns on allowBypass for them, which lets any app opt out of the VPN.
     */
    private static void startVpn(Context context, String config, String name, String username,
                                 String password, List<String> bypassPackages) throws RemoteException {
        if (config == null || config.isEmpty()) {
            throw new RemoteException("config is empty");
        }
        try {
            ConfigParser parser = new ConfigParser();
            parser.parseConfig(new StringReader(config));
            VpnProfile profile = parser.convertProfile();
            profile.mName = name;
            int check = profile.checkProfile(context);
            if (check != R.string.no_error_found) {
                throw new RemoteException(context.getString(check));
            }
            profile.mProfileCreator = context.getPackageName();
            profile.mUsername = username;
            profile.mPassword = password;
            if (bypassPackages != null && !bypassPackages.isEmpty()) {
                profile.mAllowedAppsVpnAreDisallowed = true;
                profile.mAllowAppVpnBypass = false;
                profile.mAllowedAppsVpn = new HashSet<>(bypassPackages);
            }
            ProfileManager.setTemporaryProfile(context, profile);
            VPNLaunchHelper.startOpenVpn(profile, context);
        } catch (IOException | ConfigParser.ConfigParseError e) {
            throw new RemoteException(e.getMessage());
        }
    }

    /** Remembers a start that waits for the VPN consent dialog. Replaces an older one. */
    public static synchronized void setPendingStart(String config, String name, String username,
                                                    String password, List<String> bypassPackages,
                                                    PendingStartCallback callback) {
        PendingStart previous = pendingStart;
        pendingStart = new PendingStart(config, name, username, password, bypassPackages, callback);
        if (previous != null && previous.callback != null) {
            final PendingStartCallback old = previous.callback;
            mainHandler.post(() -> old.onResolved(false, "Superseded by a newer connect request"));
        }
    }

    /** Clears a pending start and tells its caller it did not start. */
    public static void cancelPendingStart(String reason) {
        PendingStart previous;
        synchronized (OpenVpnRuntime.class) {
            previous = pendingStart;
            pendingStart = null;
        }
        if (previous != null && previous.callback != null) {
            final PendingStartCallback old = previous.callback;
            mainHandler.post(() -> old.onResolved(false, reason));
        }
    }

    /** Resolves a pending start after the VPN consent dialog. Returns false if none was pending. */
    public static boolean onPermissionResult(boolean granted) {
        final PendingStart request;
        synchronized (OpenVpnRuntime.class) {
            request = pendingStart;
            pendingStart = null;
        }
        if (request == null) return false;
        if (!granted) {
            updateStage("denied");
            if (request.callback != null) {
                mainHandler.post(() -> request.callback.onResolved(false, null));
            }
            return true;
        }
        final Context context = appContext;
        new Thread(() -> {
            String error = null;
            try {
                start(context, request.config, request.name, request.username,
                        request.password, request.bypassPackages, request.generation);
            } catch (Throwable t) {
                error = t.getMessage() != null ? t.getMessage() : t.toString();
                Log.e(TAG, "start after permission failed: " + error);
            }
            final String finalError = error;
            if (request.callback != null) {
                mainHandler.post(() -> request.callback.onResolved(finalError == null, finalError));
            }
        }, "openvpn-start").start();
        return true;
    }

    /**
     * Stops the connection through the OpenVPN service and waits until the library reports
     * that the process is gone. Returns true only when verified down. Off the main thread.
     */
    public static boolean stop(long timeoutMs) {
        stopGeneration.incrementAndGet();
        cancelPendingStart("Stopped");
        long deadline = SystemClock.elapsedRealtime() + Math.max(0, timeoutMs);
        try {
            if (!operationLock.tryLock(Math.max(0, timeoutMs), TimeUnit.MILLISECONDS)) return false;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
        try {
            if (STATE_DOWN.equals(getState())) {
                if (!"disconnected".equals(stage)) updateStage("disconnected");
                return true;
            }
            updateStage("disconnecting");

            boolean requested = requestStopViaService(Math.min(2000L, Math.max(0, timeoutMs / 2)));
            if (!requested) {
                killProcess();
            }
            // The stop request covers a start that has not reported any state yet.
            startRequestedAt = 0;
            long gracefulDeadline = Math.min(deadline,
                    SystemClock.elapsedRealtime() + Math.max(0, timeoutMs / 2));
            if (!waitUntilDown(gracefulDeadline) && requested) {
                Log.w(TAG, "stop - graceful stop timed out, killing the OpenVPN process");
                killProcess();
            }
            boolean down = waitUntilDown(deadline);
            if (down) {
                if (!"disconnected".equals(stage)) updateStage("disconnected");
            } else {
                Log.e(TAG, "stop - OpenVPN still running after " + timeoutMs + " ms");
                updateStage(mapStage(rawState, level));
            }
            return down;
        } finally {
            operationLock.unlock();
        }
    }

    private static boolean waitUntilDown(long deadline) {
        while (!STATE_DOWN.equals(getState())) {
            if (SystemClock.elapsedRealtime() >= deadline) return false;
            try {
                Thread.sleep(100L);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return STATE_DOWN.equals(getState());
            }
        }
        return true;
    }

    private static void killProcess() {
        try {
            OpenVPNThread.stop();
        } catch (Throwable t) {
            // Throws when the library never started a process in this app process.
            Log.w(TAG, "OpenVPNThread.stop failed: " + t);
        }
    }

    // Binds to the running service without creating it and asks it to stop.
    private static boolean requestStopViaService(long bindTimeoutMs) {
        Context context = appContext;
        if (context == null) return false;
        final CountDownLatch latch = new CountDownLatch(1);
        final AtomicReference<IBinder> binderRef = new AtomicReference<>();
        ServiceConnection connection = new ServiceConnection() {
            @Override
            public void onServiceConnected(ComponentName name, IBinder service) {
                binderRef.set(service);
                latch.countDown();
            }

            @Override
            public void onServiceDisconnected(ComponentName name) {
            }
        };
        Intent intent = new Intent(context, OpenVPNService.class);
        intent.setAction(OpenVPNService.START_SERVICE);
        boolean bound = false;
        try {
            bound = context.bindService(intent, connection, 0);
            if (!bound) return false;
            if (!latch.await(bindTimeoutMs, TimeUnit.MILLISECONDS)) return false;
            IBinder binder = binderRef.get();
            if (binder instanceof OpenVPNService.LocalBinder) {
                OpenVPNService service = ((OpenVPNService.LocalBinder) binder).getService();
                return service.stopVPN(false);
            }
            return false;
        } catch (Throwable t) {
            Log.w(TAG, "requestStopViaService failed: " + t);
            return false;
        } finally {
            if (bound) {
                try {
                    context.unbindService(connection);
                } catch (Throwable ignored) {
                }
            }
        }
    }

    private static void onLibraryState(String state, ConnectionStatus newLevel) {
        rawState = state != null ? state : "";
        level = newLevel != null ? newLevel : ConnectionStatus.UNKNOWN_LEVEL;
        if (level != ConnectionStatus.LEVEL_NOTCONNECTED || !"NOPROCESS".equalsIgnoreCase(rawState)) {
            // Any real progress from the library ends the start grace period.
            startRequestedAt = 0;
        }
        if (level == ConnectionStatus.LEVEL_CONNECTED) {
            if (connectedSinceEpochMs == 0) connectedSinceEpochMs = System.currentTimeMillis();
        } else if (STATE_DOWN.equals(getState())) {
            connectedSinceEpochMs = 0;
        }
        updateStage(mapStage(rawState, level));
    }

    // Same stage names the original VPNHelper produced, with terminal states taken from the
    // library's connection level so they are truthful.
    static String mapStage(String raw, ConnectionStatus currentLevel) {
        String upper = raw == null ? "" : raw.toUpperCase(Locale.ROOT);
        if ("EXITING".equals(upper)) return "exiting";
        if ("USER_VPN_PERMISSION_CANCELLED".equals(upper)) return "denied";
        if (currentLevel == ConnectionStatus.LEVEL_CONNECTED) return "connected";
        if (currentLevel == ConnectionStatus.LEVEL_NOTCONNECTED) return "disconnected";
        if (currentLevel == ConnectionStatus.LEVEL_AUTH_FAILED) return "error";
        switch (upper) {
            case "WAIT":
                return "wait_connection";
            case "AUTH":
                return "authenticating";
            case "RECONNECTING":
                return "reconnect";
            case "NONETWORK":
                return "no_connection";
            case "CONNECTING":
                return "connecting";
            case "PREPARE":
                return "prepare";
            case "":
                return "connecting";
            default:
                return upper.toLowerCase(Locale.ROOT);
        }
    }

    private static void updateStage(String newStage) {
        if (newStage == null || newStage.equals(stage)) return;
        stage = newStage;
        final String value = newStage;
        mainHandler.post(() -> {
            for (StageListener listener : listeners) {
                try {
                    listener.onStageChanged(value);
                } catch (Throwable t) {
                    Log.w(TAG, "listener failed: " + t.getMessage());
                }
            }
        });
    }
}
