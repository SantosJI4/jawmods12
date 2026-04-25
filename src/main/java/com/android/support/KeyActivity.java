package com.android.support;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

public class KeyActivity extends Activity {

    // ══════════════════════════════════════
    // CONFIGURE YOUR SERVER URL HERE
    // ══════════════════════════════════════
    private static final String SERVER_URL = "https://jawmods.squareweb.app";

    private static final String PREFS_NAME = "jawmods_prefs";
    private static final String KEY_SAVED = "saved_key";
    private static final String KEY_EXPIRES = "key_expires";

    private EditText etKey;
    private TextView tvKeyStatus;
    private ProgressBar progressKey;
    private Button btnValidate;
    private Handler handler;

    @Override
    public void onPointerCaptureChanged(boolean hasCapture) {
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Check if key is already validated and not expired
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        String savedKey = prefs.getString(KEY_SAVED, "");
        long expiresAt = prefs.getLong(KEY_EXPIRES, 0);

        if (!savedKey.isEmpty() && System.currentTimeMillis() < expiresAt) {
            // Key still valid, go straight to main
            goToMain();
            return;
        }

        setContentView(R.layout.activity_key);

        etKey = (EditText) findViewById(R.id.etKey);
        tvKeyStatus = (TextView) findViewById(R.id.tvKeyStatus);
        progressKey = (ProgressBar) findViewById(R.id.progressKey);
        btnValidate = (Button) findViewById(R.id.btnValidate);
        handler = new Handler(Looper.getMainLooper());

        // Pre-fill saved key if exists
        if (!savedKey.isEmpty()) {
            etKey.setText(savedKey);
        }

        btnValidate.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onValidateClicked();
            }
        });
    }

    private void onValidateClicked() {
        final String key = etKey.getText().toString().trim().toUpperCase();
        if (key.isEmpty()) {
            showError("Enter your license key");
            return;
        }

        // Show loading
        btnValidate.setEnabled(false);
        btnValidate.setText("VALIDATING...");
        progressKey.setVisibility(View.VISIBLE);
        tvKeyStatus.setVisibility(View.GONE);

        new Thread(new Runnable() {
            @Override
            public void run() {
                validateKeyOnServer(key);
            }
        }).start();
    }

    private void validateKeyOnServer(final String key) {
        try {
            // Get device identifiers for HWID binding
            String androidId = android.provider.Settings.Secure.getString(
                    getContentResolver(), android.provider.Settings.Secure.ANDROID_ID);
            String deviceModel = android.os.Build.MODEL;

            // Build JSON payload
            String json = "{\"key\":\"" + escapeJson(key)
                    + "\",\"android_id\":\"" + escapeJson(androidId)
                    + "\",\"device_model\":\"" + escapeJson(deviceModel) + "\"}";

            URL url = new URL(SERVER_URL + "/validate");
            HttpURLConnection conn = (HttpURLConnection) url.openConnection();
            conn.setRequestMethod("POST");
            conn.setRequestProperty("Content-Type", "application/json");
            conn.setConnectTimeout(10000);
            conn.setReadTimeout(10000);
            conn.setDoOutput(true);

            OutputStream os = conn.getOutputStream();
            os.write(json.getBytes("UTF-8"));
            os.close();

            int responseCode = conn.getResponseCode();
            BufferedReader reader;
            if (responseCode >= 200 && responseCode < 400) {
                reader = new BufferedReader(new InputStreamReader(conn.getInputStream()));
            } else {
                reader = new BufferedReader(new InputStreamReader(conn.getErrorStream()));
            }

            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                sb.append(line);
            }
            reader.close();
            conn.disconnect();

            final String response = sb.toString();

            // Simple JSON parsing (no external libs needed for Java 7)
            final boolean valid = response.contains("\"valid\":true") || response.contains("\"valid\": true");

            if (valid) {
                // Extract remaining_seconds
                long remainingSeconds = extractLong(response, "remaining_seconds");
                long expiresMs = System.currentTimeMillis() + (remainingSeconds * 1000);

                // Save key + expiration
                SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
                SharedPreferences.Editor editor = prefs.edit();
                editor.putString(KEY_SAVED, key);
                editor.putLong(KEY_EXPIRES, expiresMs);
                editor.apply();

                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        showSuccess("Key activated!");
                        // Delay transition for visual feedback
                        handler.postDelayed(new Runnable() {
                            @Override
                            public void run() {
                                goToMain();
                            }
                        }, 800);
                    }
                });
            } else {
                // Extract error message
                String error = extractString(response, "error");
                if (error.isEmpty()) {
                    error = "Invalid key";
                }
                final String errorMsg = error;
                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        showError(errorMsg);
                        resetButton();
                    }
                });
            }

        } catch (final Exception e) {
            handler.post(new Runnable() {
                @Override
                public void run() {
                    String msg = e.getMessage();
                    if (msg != null && msg.contains("Unable to resolve host")) {
                        showError("No internet connection");
                    } else {
                        showError("Connection failed: " + (msg != null ? msg : "unknown error"));
                    }
                    resetButton();
                }
            });
        }
    }

    private void goToMain() {
        Intent intent = new Intent(KeyActivity.this, MainActivity.class);
        startActivity(intent);
        finish();
        overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out);
    }

    private void showError(String msg) {
        progressKey.setVisibility(View.GONE);
        tvKeyStatus.setVisibility(View.VISIBLE);
        tvKeyStatus.setTextColor(getResources().getColor(R.color.red_error));
        tvKeyStatus.setText(msg);
    }

    private void showSuccess(String msg) {
        progressKey.setVisibility(View.GONE);
        tvKeyStatus.setVisibility(View.VISIBLE);
        tvKeyStatus.setTextColor(getResources().getColor(R.color.green_primary));
        tvKeyStatus.setText(msg);
    }

    private void resetButton() {
        btnValidate.setEnabled(true);
        btnValidate.setText("ACTIVATE");
    }

    // ══════════════════════════════════════
    // Simple JSON helpers (no external libs)
    // ══════════════════════════════════════

    private static String escapeJson(String s) {
        if (s == null) return "";
        return s.replace("\\", "\\\\").replace("\"", "\\\"");
    }

    private static long extractLong(String json, String key) {
        try {
            String search = "\"" + key + "\":";
            int idx = json.indexOf(search);
            if (idx < 0) {
                search = "\"" + key + "\": ";
                idx = json.indexOf(search);
            }
            if (idx < 0) return 0;
            int start = idx + search.length();
            // skip whitespace
            while (start < json.length() && json.charAt(start) == ' ') start++;
            int end = start;
            while (end < json.length() && Character.isDigit(json.charAt(end))) end++;
            if (end == start) return 0;
            return Long.parseLong(json.substring(start, end));
        } catch (Exception e) {
            return 0;
        }
    }

    private static String extractString(String json, String key) {
        try {
            String search = "\"" + key + "\":\"";
            int idx = json.indexOf(search);
            if (idx < 0) {
                search = "\"" + key + "\": \"";
                idx = json.indexOf(search);
            }
            if (idx < 0) return "";
            int start = idx + search.length();
            int end = json.indexOf("\"", start);
            if (end < 0) return "";
            return json.substring(start, end);
        } catch (Exception e) {
            return "";
        }
    }
}
