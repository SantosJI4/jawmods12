#include "Rect.h"
#include "Quaternion.h"
#include "Vector2.h"
#include "Vector3.h"
#include "Color.h"
#include <vector>
#include <stdexcept>

using namespace std;

struct LayerMask {
    int mask;
    
    LayerMask() {
        this->mask = 0;
    }
public:
    int get_value() {
        return mask;
    }
    void set_value(int value) {
        mask = value;
    }
};

struct RaycastHit {
    Vector3 m_Point;
    Vector3 m_Normal;
    uint32_t m_FaceID;
    float m_Distance;
    Vector2 m_UV;
    int m_Collider;
};

struct Ray {
    Vector3 m_Origin;
    Vector3 m_Direction;
	
public:
	Vector3 get_Origin() {
		return this->m_Origin;
	}
	Vector3 get_Direction() {
		return this->m_Direction;
	}
	void set_Origin(Vector3 value) {
		this->m_Origin = value;
	}
	void set_Direction(Vector3 value) {
		this->m_Direction = value;
	}
	Vector3 GetPoint(float distance) {
		return this->m_Origin + this->m_Direction * distance;
	}
	string ToString() {
		string text = "";
		text += "Origin(";
		text += to_string(this->m_Origin.x) + ", ";
		text += to_string(this->m_Origin.y) + ", ";
		text += to_string(this->m_Origin.z) + ")\n";
		text += "Direction(";
		text += to_string(this->m_Direction.x) + ", ";
		text += to_string(this->m_Direction.y) + ", ";
		text += to_string(this->m_Direction.z) + ")";
		return text;
	}
	
};