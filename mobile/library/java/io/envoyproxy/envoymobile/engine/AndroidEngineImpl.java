package io.envoyproxy.envoymobile.engine;

import android.content.Context;
import android.net.ConnectivityManager;

import io.envoyproxy.envoymobile.engine.types.EnvoyEventTracker;
import io.envoyproxy.envoymobile.engine.types.EnvoyHTTPCallbacks;
import io.envoyproxy.envoymobile.engine.types.EnvoyLogger;
import io.envoyproxy.envoymobile.engine.types.EnvoyConnectionType;
import io.envoyproxy.envoymobile.engine.types.EnvoyOnEngineRunning;
import io.envoyproxy.envoymobile.engine.types.EnvoyStringAccessor;
import io.envoyproxy.envoymobile.engine.types.EnvoyStatus;
import io.envoyproxy.envoymobile.utilities.ContextUtils;

import java.util.Map;

/* Android-specific implementation of the `EnvoyEngine` interface. */
public class AndroidEngineImpl implements EnvoyEngine {
  private final EnvoyEngine envoyEngine;
  private final Context context;

  /**
   * @param runningCallback Called when the engine finishes its async startup and begins running.
   */
  public AndroidEngineImpl(Context context, EnvoyOnEngineRunning runningCallback,
                           EnvoyLogger logger, EnvoyEventTracker eventTracker,
                           Boolean enableProxying, Boolean useNetworkChangeEvent,
                           Boolean disableDnsRefreshOnNetworkChange) {
    this.context = context;
    this.envoyEngine = new EnvoyEngineImpl(runningCallback, logger, eventTracker,
                                           disableDnsRefreshOnNetworkChange);
    if (ContextUtils.getApplicationContext() == null) {
      ContextUtils.initApplicationContext(context.getApplicationContext());
    }
    AndroidNetworkMonitor.load(context, envoyEngine, useNetworkChangeEvent);
    if (enableProxying) {
      AndroidProxyMonitor.load(context, envoyEngine);
    }
  }

  @Override
  public EnvoyHTTPStream startStream(EnvoyHTTPCallbacks callbacks, boolean explicitFlowControl) {
    return envoyEngine.startStream(callbacks, explicitFlowControl);
  }

  @Override
  public void performRegistration(EnvoyConfiguration envoyConfiguration) {
    envoyEngine.performRegistration(envoyConfiguration);
  }

  @Override
  public EnvoyStatus runWithConfig(EnvoyConfiguration envoyConfiguration, String logLevel) {
    return envoyEngine.runWithConfig(envoyConfiguration, logLevel);
  }

  @Override
  public void terminate() {
    envoyEngine.terminate();
  }

  @Override
  public String dumpStats() {
    return envoyEngine.dumpStats();
  }

  @Override
  public int recordCounterInc(String elements, Map<String, String> tags, int count) {
    return envoyEngine.recordCounterInc(elements, tags, count);
  }

  @Override
  public int registerStringAccessor(String accessorName, EnvoyStringAccessor accessor) {
    return envoyEngine.registerStringAccessor(accessorName, accessor);
  }

  @Override
  public void resetConnectivityState() {
    envoyEngine.resetConnectivityState();
  }

  @Override
  public void onDefaultNetworkAvailable() {
    envoyEngine.onDefaultNetworkAvailable();
  }

  @Override
  public void onDefaultNetworkChanged(int network) {
    envoyEngine.onDefaultNetworkChanged(network);
  }

  @Override
  public void onDefaultNetworkChangeEvent(int network) {
    envoyEngine.onDefaultNetworkChangeEvent(network);
  }

  @Override
  public void onDefaultNetworkChangedV2(EnvoyConnectionType network_type, long net_id) {
    envoyEngine.onDefaultNetworkChangedV2(network_type, net_id);
  }

  @Override
  public void onNetworkDisconnect(long net_id) {
    envoyEngine.onNetworkDisconnect(net_id);
  }

  @Override
  public void onNetworkConnect(EnvoyConnectionType network_type, long net_id) {
    envoyEngine.onNetworkConnect(network_type, net_id);
  }

  @Override
  public void purgeActiveNetworkList(long[] activeNetIds) {
    envoyEngine.purgeActiveNetworkList(activeNetIds);
  }

  @Override
  public void onDefaultNetworkUnavailable() {
    envoyEngine.onDefaultNetworkUnavailable();
  }

  public void setProxySettings(String host, int port) { envoyEngine.setProxySettings(host, port); }

  public void setLogLevel(LogLevel log_level) { envoyEngine.setLogLevel(log_level); }
}
