// ============================================================
// Free Fire 1.123.1 — Offsets usados no mod (libil2cpp.so)
// Todos os offsets são relativos à base de libil2cpp.so
// ============================================================

// ── VMT Hook — ponto de entrada do hook ──────────────────────
namespace: Assembly-CSharp.dll
class: COW.GamePlay.Player
private System.Void LateUpdate(); // 0x67BE774  ← VMT hook aqui

// ── Identificação de jogadores ───────────────────────────────
namespace: Assembly-CSharp.dll
class: COW.GamePlay.Player
public System.Boolean IsLocalPlayer(); // 0x67558A4
public virtual System.Boolean IsLocalTeammate(System.Boolean checkTeamId); // 0x6789BF8

// ── HP do jogador ─────────────────────────────────────────────
namespace: Assembly-CSharp.dll
class: COW.GamePlay.Player
public System.Int32 get_CurHP(); // 0x67CDD24
public System.Int32 get_MaxHP(); // 0x67CDE1C
// campo bool IsKnockedDownBleed — offset de campo no objeto Player (não é método)
// bool IsKnockedDownBleed @ offset 0x1150

// ── Colisão / Headshot ───────────────────────────────────────
namespace: Assembly-CSharp.dll
class: COW.GamePlay.Player
public UnityEngine.Collider get_HeadCollider(); // 0x676FEB4

namespace: Assembly-CSharp.dll
class: COW.GamePlay.PlayerColliderChecker
// Determina qual parte do corpo foi atingida pela bala
// HitPart enum: Head=0, Neck=1, Chest=2, Hips=3, LeftArm=4, RightArm=6, LeftLeg=9, RightLeg=11, ...NoFlag=18
public COW.GamePlay.HitPart GetPartByCollider(UnityEngine.Collider collider); // 0x4FEAD00
// ← PATCH v30: MOV W0,#0 + RET → sempre retorna Head (0)

namespace: Assembly-CSharp.dll
class: COW.GamePlay.ELMGJKHIIAA  // DamageInfo
// Verifica se o dano é headshot. Usado no cálculo real de dano (TakeDamage).
// PATCH v30.2: MOV W0,#1 + RET → sempre retorna true = headshot garantido
public static System.Boolean HPGMEMPFIPD(COW.GamePlay.ELMGJKHIIAA damageInfo); // 0x6F43D74

// ── Aim / Câmera ─────────────────────────────────────────────
namespace: UnityEngine.CoreModule.dll
class: UnityEngine.Camera
public static UnityEngine.Camera get_main(); // 0x9C0764C
public struct UnityEngine.Vector3 WorldToScreenPoint(struct UnityEngine.Vector3 position); // 0x9C072D0
public struct UnityEngine.Matrix4x4 get_worldToCameraMatrix(); // 0x9C06D7C
public struct UnityEngine.Matrix4x4 get_projectionMatrix(); // 0x9C06E2C

namespace: UnityEngine.CoreModule.dll
class: UnityEngine.Component
public UnityEngine.Transform get_transform(); // 0x9C4DF54

namespace: UnityEngine.CoreModule.dll
class: UnityEngine.Transform
public struct UnityEngine.Vector3 get_position(); // 0x9C5C0F4
public struct UnityEngine.Vector3 get_eulerAngles(); // 0x9C5C2B4
public System.Void set_eulerAngles(struct UnityEngine.Vector3 value); // 0x9C5C33C

// ── Tela ──────────────────────────────────────────────────────
namespace: UnityEngine.CoreModule.dll
class: UnityEngine.Screen
public static System.Int32 get_width(); // 0x9C14D60
public static System.Int32 get_height(); // 0x9C14D88

// ── Animator / Bone ──────────────────────────────────────────
namespace: UnityEngine.CoreModule.dll
class: UnityEngine.Animator
// HumanBodyBones.Head = 10
public UnityEngine.Transform GetBoneTransform(UnityEngine.HumanBodyBones humanBoneId); // 0x9BEE970

// ── Collider Bounds ──────────────────────────────────────────
namespace: UnityEngine.CoreModule.dll
class: UnityEngine.Collider
// Versão Injected: recebe ponteiro de saída como parâmetro (ABI ARM64 correta para SRET)
public System.Void get_bounds_Injected(out UnityEngine.Bounds ret); // 0x9CB794C

// ── Hierarquia de classes ────────────────────────────────────
// COW.GamePlay.Player : COW.GamePlay.AttackableEntity, COW.GamePlay.EAHHEDNCJAL,
//                       COW.GamePlay.MPICKNDAPEB, COW.GamePlay.AEPAPLNNAHF,
//                       GCommon.IReusableObjectOwner, COW.GamePlay.IJOMOIINJGM,
//                       COW.GamePlay.EHCKCDIGOGA

// ── Offsets de campos no objeto Player ───────────────────────
// Player+0x700  → NewPlayerAnimationSystemComponent* (campo HFKJCLHCBGB)
// AnimSystemComp+0x28 → Animator* (campo m_Animator, base GCommon.AnimationSystemComponent)
// Player+0x1150 → bool IsKnockedDownBleed
