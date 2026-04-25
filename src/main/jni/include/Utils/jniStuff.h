#pragma once

JavaVM *jvm;
JNIEnv* getEnv() {
    JNIEnv *env;
    int status = jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if(status < 0) {
        status = jvm->AttachCurrentThread(&env, NULL);
        if(status < 0) {
            return nullptr;
        }
    }
    return env;
}

jobject getGlobalContext(JNIEnv *env)
{
    jclass activityThread = env->FindClass("android/app/ActivityThread");
    jmethodID currentActivityThread = env->GetStaticMethodID(activityThread, "currentActivityThread", "()Landroid/app/ActivityThread;");
    jobject at = env->CallStaticObjectMethod(activityThread, currentActivityThread);

    jmethodID getApplication = env->GetMethodID(activityThread, "getApplication", "()Landroid/app/Application;");
    jobject context = env->CallObjectMethod(at, getApplication);

    return context;
}

const char *getClipboardText() {
    const char *result;
    JNIEnv *env;
    
    jvm->AttachCurrentThread(&env, NULL);
    
    auto looperClass = env->FindClass("android/os/Looper");
    auto prepareMethod = env->GetStaticMethodID(looperClass, "prepare", "()V");
   
    jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
    jfieldID sCurrentActivityThreadField = env->GetStaticFieldID(activityThreadClass, "sCurrentActivityThread", "Landroid/app/ActivityThread;");
    jobject sCurrentActivityThread = env->GetStaticObjectField(activityThreadClass, sCurrentActivityThreadField);
    
    jfieldID mInitialApplicationField = env->GetFieldID(activityThreadClass, "mInitialApplication", "Landroid/app/Application;");
    jobject mInitialApplication = env->GetObjectField(sCurrentActivityThread, mInitialApplicationField);
    
    auto contextClass = env->FindClass("android/content/Context");
    auto getSystemServiceMethod = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    
    auto str = env->NewStringUTF("clipboard");
    auto clipboardManager = env->CallObjectMethod(mInitialApplication, getSystemServiceMethod, str);
  
    jclass ClipboardManagerClass = env->FindClass("android/content/ClipboardManager");
    auto getText = env->GetMethodID(ClipboardManagerClass, "getText", "()Ljava/lang/CharSequence;");

    jclass CharSequenceClass = env->FindClass("java/lang/CharSequence");
    auto toStringMethod = env->GetMethodID(CharSequenceClass, "toString", "()Ljava/lang/String;");

    auto text = env->CallObjectMethod(clipboardManager, getText);
    if (text) {
        str = (jstring) env->CallObjectMethod(text, toStringMethod);
        result = env->GetStringUTFChars(str, 0);  
    }
    return result;
}



int ShowSoftKeyboardInput() {
	jint result;
	jint flags = 0;
	
	JNIEnv *env;
	jvm->AttachCurrentThread(&env, NULL);
	
	jclass looperClass = env->FindClass("android/os/Looper");
	auto prepareMethod = env->GetStaticMethodID(looperClass, "prepare", "()V");
	env->CallStaticVoidMethod(looperClass, prepareMethod);
	
	jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
	jfieldID sCurrentActivityThreadField = env->GetStaticFieldID(activityThreadClass, "sCurrentActivityThread", "Landroid/app/ActivityThread;");
	jobject sCurrentActivityThread = env->GetStaticObjectField(activityThreadClass, sCurrentActivityThreadField);
	
	jfieldID mInitialApplicationField = env->GetFieldID(activityThreadClass, "mInitialApplication", "Landroid/app/Application;");
	jobject mInitialApplication = env->GetObjectField(sCurrentActivityThread, mInitialApplicationField);
	
	jclass contextClass = env->FindClass("android/content/Context");
	jfieldID fieldINPUT_METHOD_SERVICE = env->GetStaticFieldID(contextClass, "INPUT_METHOD_SERVICE", "Ljava/lang/String;");
	jobject INPUT_METHOD_SERVICE = env->GetStaticObjectField(contextClass, fieldINPUT_METHOD_SERVICE);
	jmethodID getSystemServiceMethod = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
	jobject callObjectMethod = env->CallObjectMethod(mInitialApplication, getSystemServiceMethod, INPUT_METHOD_SERVICE);
	
	jclass classInputMethodManager = env->FindClass("android/view/inputmethod/InputMethodManager");
    jmethodID toggleSoftInputId = env->GetMethodID(classInputMethodManager, "toggleSoftInput", "(II)V");
	
	if (result) {
		env->CallVoidMethod(callObjectMethod, toggleSoftInputId, 2, flags);
	} else {
		env->CallVoidMethod(callObjectMethod, toggleSoftInputId, flags, flags);
	}
	
	env->DeleteLocalRef(classInputMethodManager);
	env->DeleteLocalRef(callObjectMethod);
	env->DeleteLocalRef(contextClass);
    env->DeleteLocalRef(mInitialApplication);
    env->DeleteLocalRef(activityThreadClass);
	jvm->DetachCurrentThread();
	
	return result;
}





