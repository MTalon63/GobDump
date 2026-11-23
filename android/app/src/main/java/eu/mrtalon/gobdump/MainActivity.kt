package eu.mrtalon.gobdump

import android.app.NativeActivity
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.content.Context
import android.view.inputmethod.InputMethodManager
import android.view.KeyEvent
import java.util.concurrent.LinkedBlockingQueue
import android.util.Log
import android.content.res.AssetManager
import java.io.*
import java.util.concurrent.atomic.AtomicBoolean

import android.content.Intent;
import android.app.Activity;
import android.net.Uri;
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest

import android.Manifest;
import androidx.core.content.PermissionChecker;
import androidx.core.app.ActivityCompat;
import android.content.pm.PackageManager;
import android.provider.DocumentsContract;

import android.content.BroadcastReceiver;
import android.app.PendingIntent;
import android.content.IntentFilter;

import android.view.View;
import android.view.ViewGroup;
import android.view.Window;

import android.widget.RelativeLayout;
import android.widget.EditText;
import android.text.TextWatcher;
import android.text.Editable;
import android.text.InputType;

import android.view.WindowManager;

import org.woheller69.freeDroidWarn.FreeDroidWarn;
import eu.mrtalon.gobdump.BuildConfig;

// Extension on intent
fun Intent?.getFilePath(context: Context): String {
    return this?.data?.let { data -> RealPathUtil.getRealPath(context, data) ?: "" } ?: ""
}

// Extension on intent
fun Intent?.getFilePathDir(context: Context): String {
    return this?.data?.let { data -> RealPathUtil.getRealPath(context, DocumentsContract.buildDocumentUriUsingTree(data, DocumentsContract.getTreeDocumentId(data))) ?: "" } ?: ""
}

class MainActivity : NativeActivity(), TextWatcher {
    private val TAG : String = "GobDump";

    private external fun nativeOnUsbPermissionGranted()

    fun checkAndAsk(permission: String) {
        if (PermissionChecker.checkSelfPermission(this, permission) != PermissionChecker.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(permission), 1);
        }
    }

    private var ACTION_USB_PERMISSION = "libusb.android.USB_PERMISSION";

    private var usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (ACTION_USB_PERMISSION == intent.action) {
                synchronized(this) {
                    Log.i(TAG, "USB permission reply received; refreshing SDR sources")
                    nativeOnUsbPermissionGranted()
                }
            }
        }
    }

    public var mLayout : ViewGroup? = null;
    public var editText : EditText? = null;
    public var lastFiller : String? = null;

    public override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Ask for required permissions, without these the app cannot run.
        checkAndAsk(Manifest.permission.WRITE_EXTERNAL_STORAGE);
        checkAndAsk(Manifest.permission.READ_EXTERNAL_STORAGE);
        checkAndAsk(Manifest.permission.INTERNET);

        // Register events
        //        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager;
        //        val permissionIntent = PendingIntent.getBroadcast(this, 0, Intent(ACTION_USB_PERMISSION), 0)
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        // Android 13+ requires an explicit exported flag on context-registered receivers; the USB
        // permission broadcast originates outside the app, so it must be RECEIVER_EXPORTED.
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(usbReceiver, filter, Context.RECEIVER_EXPORTED)
        } else {
            registerReceiver(usbReceiver, filter)
        }

        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val ethRequest = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET)
            .build()

        ethCallback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                // Route GobDump's sockets (192.168.10.2) over eth0 even while Wi-Fi is active
                cm.bindProcessToNetwork(network)
            }

            override fun onLost(network: Network) {
                // Fall back to default routing (Wi-Fi / Mobile) when the eth adapter is unplugged
                cm.bindProcessToNetwork(null)
            }
        }
        cm.registerNetworkCallback(ethRequest, ethCallback!!)

        // Get permission for all USB devices
        // val devList = usbManager!!.getDeviceList();
        // for ((name, dev) in devList) {
        //     usbManager!!.requestPermission(dev, permissionIntent);
        // }

        // Hide system bars
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);

        // Keep screen on
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // Partial wake lock keeps the CPU running for live DSP while the screen is off; released in onDestroy().
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        cpuWakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "GobDump::DSPCompute").apply {
            acquire()
        }

        // Text crap
        mLayout = RelativeLayout(this);
        editText = EditText(this.applicationContext!!);
        mLayout!!.addView(editText, RelativeLayout.LayoutParams(10000, 10000));
        editText!!.setVisibility(View.VISIBLE);
        editText!!.setInputType(InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        editText!!.requestFocus();
        editText!!.setText(" ");
        editText!!.setSelection(1);
        lastFiller = " ";
        editText!!.addTextChangedListener(this);

        setContentView(mLayout);

        FreeDroidWarn.showWarningOnUpgrade(this, BuildConfig.VERSION_CODE);
    }
    private var ethCallback: ConnectivityManager.NetworkCallback? = null
    private var cpuWakeLock: PowerManager.WakeLock? = null

    override fun onDestroy() {
        cpuWakeLock?.let { if (it.isHeld) it.release() }
        cpuWakeLock = null
        ethCallback?.let {
            val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
            cm.unregisterNetworkCallback(it)
            cm.bindProcessToNetwork(null)
        }
        super.onDestroy()
    }

    public fun getAppDir(): String {
        val fdir = getFilesDir().getAbsolutePath();

        // Extract all resources to the app directory
        val aman = getAssets();
        extractDir(aman, fdir + "/resources", "resources");
        // extractDir(aman, fdir + "/plugins", "plugins");
        extractFile(aman, fdir + "/gobdump_cfg.json", "gobdump_cfg.json");
        //createIfDoesntExist(fdir + "/plugins");

        return fdir;
    }

    public fun get_plugins_directory() : String {
        return getApplicationInfo().nativeLibraryDir;
    }

    public fun get_dpi() : Float {
        return getResources().getDisplayMetrics().density;
    }

    fun showSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethodManager.showSoftInput(editText, 0)
    }

    fun hideSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        inputMethodManager.hideSoftInputFromWindow(editText!!.windowToken, 0)
    }

    // Queue for the Unicode characters to be polled from native code (via pollUnicodeChar())
    private var unicodeCharacterQueue: LinkedBlockingQueue<Int> = LinkedBlockingQueue()

    // Not all Android keyboard trigger a KeyEvent
    // so I had to get around it somehow...
    // I'm not super proud of this and it has downsides,
    // but at least it works!
    // Hecking Android not having a simple function
    // to get Key events......... WHY!?
    override fun afterTextChanged(s : Editable) {
        if(!editText!!.getText().toString().startsWith(" "))
        {
            editText!!.setText(" ");
            editText!!.setSelection(1);
        }

        lastFiller = editText!!.getText().toString();
    }

    override fun beforeTextChanged(s : CharSequence, start: Int, count: Int, after: Int) {
    }

    override fun onTextChanged(s : CharSequence, start: Int, before: Int, count: Int) {
        if(editText!!.getText().toString() != lastFiller) {
            if(before < count) {
                var char2 = s.get(s.length - 1);
                unicodeCharacterQueue.offer(char2.toInt());
            }
	    // Backspace is already handled via AKEYCODE_DEL in ImGui_ImplAndroid_HandleInputEvent
        }
    }

    fun pollUnicodeChar(): Int {
        return unicodeCharacterQueue.poll() ?: 0
    }

    public fun extractFile(aman: AssetManager, local: String, rsrc: String): Int {
        val lpath = local;
        val rpath = rsrc;

        Log.w(TAG, "Extracting '" + rpath + "' to '" + lpath + "'");

        // This is a file, extract it
        val _os = FileOutputStream(lpath);
        val _is = aman.open(rpath);
        val ilen = _is.available();
        var fbuf = ByteArray(ilen);
        _is.read(fbuf, 0, ilen);
        _os.write(fbuf);
        _os.close();
        _is.close();

        return 0;
    }

    public fun extractDir(aman: AssetManager, local: String, rsrc: String): Int {
        val flist = aman.list(rsrc);
        var ecount = 0;
        for (fp in flist!!) {
            val lpath = local + "/" + fp;
            val rpath = rsrc + "/" + fp;

            Log.w(TAG, "Extracting '" + rpath + "' to '" + lpath + "'");

            // Create local path if non-existent
            createIfDoesntExist(local);
            
            // Create if directory
            val ext = extractDir(aman, lpath, rpath);

            // Extract if file
            if (ext == 0) {
                // This is a file, extract it
                val _os = FileOutputStream(lpath);
                val _is = aman.open(rpath);
                val ilen = _is.available();
                var fbuf = ByteArray(ilen);
                _is.read(fbuf, 0, ilen);
                _os.write(fbuf);
                _os.close();
                _is.close();
            }

            ecount++;
        }
        return ecount;
    }

    public fun createIfDoesntExist(path: String) {
        // This is a directory, create it in the filesystem
        var folder = File(path);
        var success = true;
        if (!folder.exists()) {
            success = folder.mkdirs();
        }
        if (!success) {
            Log.e(TAG, "Could not create folder with path " + path);
        }
    }

    // Handle selecting a file
    var select_file_result : String = "";
    public fun select_file() {
        var file_intent = Intent(Intent.ACTION_GET_CONTENT);
        file_intent.setType("*/*");
        file_intent.addCategory(Intent.CATEGORY_OPENABLE);
        val final_intent = Intent.createChooser(file_intent, "Select File");
        startActivityForResult(final_intent, 1);
    }

    public fun select_file_get() : String {
        var tmp = select_file_result;
        select_file_result = "";
        return tmp;
    }

    // Handle selecting a directory
    var select_directory_result : String = "";
    public fun select_directory() {
        var file_intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        file_intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        file_intent.addCategory(Intent.CATEGORY_DEFAULT);
        val final_intent = Intent.createChooser(file_intent, "Select Directory");
        startActivityForResult(final_intent, 2);
    }

    public fun select_directory_get() : String {
        var tmp = select_directory_result;
        select_directory_result = "";
        return tmp;
    }

    // Handle savinf a file
    var select_filesave_result : String = "";
    public fun select_filesave(name: String) {
        var file_intent = Intent(Intent.ACTION_CREATE_DOCUMENT);
        file_intent.setType("*/*");
        file_intent.putExtra(Intent.EXTRA_TITLE, name);
        val final_intent = Intent.createChooser(file_intent, "Select Destination File");
        startActivityForResult(final_intent, 3);
    }

    public fun select_filesave_get() : String {
        var tmp = select_filesave_result;
        select_filesave_result = "";
        return tmp;
    }

    public fun openURL(url: String) {
        val browserIntent = Intent(Intent.ACTION_VIEW, Uri.parse(url));
        startActivity(browserIntent);
    }

    // Receive results of the above
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data);

        try {
            if (requestCode == 1) {
                if(resultCode == RESULT_OK)
                    select_file_result = data.getFilePath(getApplicationContext());
                else if(resultCode == RESULT_CANCELED)
                    select_file_result = "NO_PATH_SELECTED";
            }

            if (requestCode == 2) {
                if(resultCode == RESULT_OK)
                    select_directory_result = data.getFilePathDir(getApplicationContext());
                else if(resultCode == RESULT_CANCELED)
                    select_directory_result = "NO_PATH_SELECTED";
            }

            if (requestCode == 3) {
                if(resultCode == RESULT_OK)
                    select_filesave_result = data.getFilePath(getApplicationContext());
                else if(resultCode == RESULT_CANCELED)
                    select_filesave_result = "NO_PATH_SELECTED";
            } 
        } catch (e: java.lang.RuntimeException) {
            Log.w(TAG, "Error! " + e.message);
        }
    }
}
