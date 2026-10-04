#include <jni.h>
#include <whisper.h>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>

extern "C" JNIEXPORT jstring JNICALL Java_com_hr54_controller_speech_whisper_WhisperEngine_transcribeNative(JNIEnv * env, jobject, jstring path, jfloatArray audio) {
 const char * model = env->GetStringUTFChars(path, nullptr);
 auto params = whisper_context_default_params(); params.use_gpu = false;
 auto * ctx = whisper_init_from_file_with_params(model, params);
 env->ReleaseStringUTFChars(path, model);
 if (!ctx) { env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "Unable to load bundled Whisper model"); return nullptr; }
 const int n = env->GetArrayLength(audio); std::vector<float> pcm(n); env->GetFloatArrayRegion(audio,0,n,pcm.data());
 auto options=whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
 options.n_threads=std::min(4,std::max(1,static_cast<int>(std::thread::hardware_concurrency())));
 options.language="en"; options.translate=false; options.no_context=true; options.single_segment=true;
 options.print_progress=false; options.print_realtime=false; options.print_timestamps=false; options.print_special=false;
 const int result=whisper_full(ctx,options,pcm.data(),n); std::string text;
 if (result==0) for(int i=0;i<whisper_full_n_segments(ctx);i++) text+=whisper_full_get_segment_text(ctx,i);
 whisper_free(ctx);
 if(result!=0) { env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),"Local Whisper transcription failed"); return nullptr; }
 // JNI NewStringUTF uses modified UTF-8. Convert actual UTF-8 through Java's charset decoder.
 jbyteArray bytes=env->NewByteArray(text.size()); env->SetByteArrayRegion(bytes,0,text.size(),reinterpret_cast<const jbyte *>(text.data()));
 jclass cls=env->FindClass("java/lang/String"); jmethodID ctor=env->GetMethodID(cls,"<init>","([BLjava/lang/String;)V");
 return static_cast<jstring>(env->NewObject(cls,ctor,bytes,env->NewStringUTF("UTF-8")));
}
