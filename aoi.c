#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include "aoi.h"

#define AOI_RADIS 10.0f

#define INVALID_ID (~0)
#define PRE_ALLOC 16
// 比较时用这个 2 次方的值，因为下面的计算出来的距离也没有开根号
#define AOI_RADIS2 (AOI_RADIS * AOI_RADIS)
// 三维空间的 2 点间距离公式。但不开根号，避免性能损失。
#define DIST2(p1,p2) ((p1[0] - p2[0]) * (p1[0] - p2[0]) + (p1[1] - p2[1]) * (p1[1] - p2[1]) + (p1[2] - p2[2]) * (p1[2] - p2[2]))
#define MODE_WATCHER 1
#define MODE_MARKER 2
#define MODE_MOVE 4
#define MODE_DROP 8

struct object {
	int ref;
	uint32_t id;
	int version;
	int mode;
	float last[3];
	float position[3];
};

struct object_set {
	int cap; // 容量
	int number; // 实际数量
	struct object ** slot; // 连续的实体对象指针
};

struct pair_list {
	struct pair_list * next;
	struct object * watcher;
	struct object * marker;
	int watcher_version;
	int marker_version;
};

// 其中一个 hash 槽
struct map_slot {
	uint32_t id; // 实体对象 id
	struct object * obj; // 指向的实体对象
	int next; // 下一个实体所在索引。发生 hash 冲突时这个值才有意义，否则都是 -1
};

// 哈希表，存放实体对象相关信息
struct map {
	int size; // 下面那堆 slot 的个数
	int lastfree; // 最后一个空闲未用 slot 的索引
	struct map_slot * slot; // 一堆连续的哈希 slot，一个 slot 存一个 实体对象信息
};

struct aoi_space {
	aoi_Alloc alloc;
	void * alloc_ud;
	struct map * object; // 哈希表，存放实体对象相关；变量名字起得和另一个结构体名竟然相同！！
	struct object_set * watcher_static;
	struct object_set * marker_static;
	struct object_set * watcher_move;
	struct object_set * marker_move;
	struct pair_list * hot; // 热点对
};

static struct object *
new_object(struct aoi_space * space, uint32_t id) {
	struct object * obj = space->alloc(space->alloc_ud, NULL, sizeof(*obj));
	obj->ref = 1;
	obj->id = id;
	obj->version = 0;
	obj->mode = 0;
	return obj;
}

// 根据实体对象的 id 找到相应的 slot;用的算是 hash 算法了
static inline struct map_slot *
mainposition(struct map *m , uint32_t id) {
	uint32_t hash = id & (m->size-1); // 高科技啊! size-1 值的二进制刚好是低位全是 1
	return &m->slot[hash];
}

static void rehash(struct aoi_space * space, struct map *m);

static void
map_insert(struct aoi_space * space , struct map * m, uint32_t id , struct object *obj) {
	struct map_slot *s = mainposition(m,id);
	if (s->id == INVALID_ID) { // 刚好是空闲的
		s->id = id;
		s->obj = obj;
		return;
	}
	// 重复插入 ？？？

	// 发现了占座位现象，旧对象得让座 （不能  s->id != id 这样比来检查非法占座位，不同的 id 想占同一个座位说明是发生哈希冲突，是合理现象）
	// 
	if (mainposition(m, s->id) != s) {
		struct map_slot * last = mainposition(m,s->id); // 原本应该所在座位，就是头节点

		// 找出谁指向这个座位
		while (last->next != s - m->slot) {
			assert(last->next >= 0);
			last = &m->slot[last->next];
		}
		// 暂存旧对象
		uint32_t temp_id = s->id;
		struct object * temp_obj = s->obj;

		// 链回去， s 出链
		last->next = s->next;

		// 新对象入座位
		s->id = id;
		s->obj = obj;
		s->next = -1;

		// 旧对象重新找个座位
		if (temp_obj) {
			map_insert(space, m, temp_id, temp_obj);
		}
		return;
	}
	// 处理哈希冲突（多个人想坐一个座位），找个空闲的 slot 占座（从后往前找）
	// 这里乱占座位的行为，会导致占了本应该是别人的座位，只是主人还没有入座，后面要是主人来了还得让座
	while (m->lastfree >= 0) {
		struct map_slot * temp = &m->slot[m->lastfree--];
		if (temp->id == INVALID_ID) {
			temp->id = id;
			temp->obj = obj;

			// 插在前面的第 2 位（逻辑意义上的 “前”，插在第 2 位纯属好实现而已）
			temp->next = s->next;
			s->next = (int)(temp - m->slot);
			return;
		}
	}

	// 空闲的 slot 不足了，重新哈希后再插入
	rehash(space,m);
	map_insert(space, m, id , obj);
}

static void
rehash(struct aoi_space * space, struct map *m) {
	struct map_slot * old_slot = m->slot;
	int old_size = m->size;
	m->size = 2 * old_size;
	m->lastfree = m->size - 1;
	m->slot = space->alloc(space->alloc_ud, NULL, m->size * sizeof(struct map_slot));
	int i;

	// 对一堆新的 slot 进行初始化
	for (i=0;i<m->size;i++) {
		struct map_slot * s = &m->slot[i];
		s->id = INVALID_ID;
		s->obj = NULL;
		s->next = -1;
	}
	// 把旧的 实体对象 插入到新的一堆 slot 中
	for (i=0;i<old_size;i++) {
		struct map_slot * s = &old_slot[i];
		if (s->obj) {
			map_insert(space, m, s->id, s->obj);
		}
	}
	// free 旧内存
	space->alloc(space->alloc_ud, old_slot, old_size * sizeof(struct map_slot));
}

// 查不到会进行插入！！！
static struct object *
map_query(struct aoi_space *space, struct map * m, uint32_t id) {
	struct map_slot *s = mainposition(m, id);
	for (;;) {
		if (s->id == id) {
			if (s->obj == NULL) {
				s->obj = new_object(space, id);
			}
			return s->obj;
		}
		// 走到这里，都是发生了哈希冲突的

		// 向后找（逻辑意义上的后）
		if (s->next < 0) {
			break;
		}
		s=&m->slot[s->next];
	}
	struct object * obj = new_object(space, id);
	map_insert(space, m , id , obj);
	return obj;
}

static void
map_foreach(struct map * m , void (*func)(void *ud, struct object *obj), void *ud) {
	int i;
	for (i=0;i<m->size;i++) {
		if (m->slot[i].obj) {
			func(ud, m->slot[i].obj);
		}
	}
}

static struct object *
map_drop(struct map *m, uint32_t id) {
	uint32_t hash = id & (m->size-1);
	struct map_slot *s = &m->slot[hash];
	for (;;) {
		if (s->id == id) {
			struct object * obj = s->obj;
			// 置空即可，并不是真删
			s->obj = NULL;
			return obj;
		}
		if (s->next < 0) {
			return NULL;
		}
		s=&m->slot[s->next];
	}
}

static void
map_delete(struct aoi_space *space, struct map * m) {
	space->alloc(space->alloc_ud, m->slot, m->size * sizeof(struct map_slot));
	space->alloc(space->alloc_ud, m , sizeof(*m));
}

static struct map *
map_new(struct aoi_space *space) {
	int i;
	struct map * m = space->alloc(space->alloc_ud, NULL, sizeof(*m));
	m->size = PRE_ALLOC;
	m->lastfree = PRE_ALLOC - 1;
	m->slot = space->alloc(space->alloc_ud, NULL, m->size * sizeof(struct map_slot));
	for (i=0;i<m->size;i++) {
		struct map_slot * s = &m->slot[i];
		s->id = INVALID_ID;
		s->obj = NULL;
		s->next = -1;
	}
	return m;
}

// 增加引用计数
inline static void
grab_object(struct object *obj) {
	++obj->ref;
}

static void
delete_object(void *s, struct object * obj) {
	struct aoi_space * space = s;
	space->alloc(space->alloc_ud, obj, sizeof(*obj));
}

inline static void
drop_object(struct aoi_space * space, struct object *obj) {
	--obj->ref;
	if (obj->ref <=0) {
		map_drop(space->object, obj->id);
		delete_object(space, obj);
	}
}

static struct object_set *
set_new(struct aoi_space * space) {
	struct object_set * set = space->alloc(space->alloc_ud, NULL, sizeof(*set));
	set->cap = PRE_ALLOC;
	set->number = 0;
	set->slot = space->alloc(space->alloc_ud, NULL, set->cap * sizeof(struct object *));
	return set;
}

struct aoi_space * 
aoi_create(aoi_Alloc alloc, void *ud) {
	struct aoi_space *space = alloc(ud, NULL, sizeof(*space));
	space->alloc = alloc;
	space->alloc_ud = ud;
	space->object = map_new(space);
	space->watcher_static = set_new(space);
	space->marker_static = set_new(space);
	space->watcher_move = set_new(space);
	space->marker_move = set_new(space);
	space->hot = NULL;
	return space;
}

static void
delete_pair_list(struct aoi_space * space) {
	struct pair_list * p = space->hot;
	while (p) {
		struct pair_list * next = p->next;
		space->alloc(space->alloc_ud, p, sizeof(*p));
		p = next;
	}
}

static void
delete_set(struct aoi_space *space, struct object_set * set) {
	if (set->slot) {
		space->alloc(space->alloc_ud, set->slot, sizeof(struct object *) * set->cap);
	}
	space->alloc(space->alloc_ud, set, sizeof(*set));
}

void 
aoi_release(struct aoi_space *space) {
	map_foreach(space->object, delete_object, space);
	map_delete(space, space->object);
	delete_pair_list(space);
	delete_set(space,space->watcher_static);
	delete_set(space,space->marker_static);
	delete_set(space,space->watcher_move);
	delete_set(space,space->marker_move);
	space->alloc(space->alloc_ud, space, sizeof(*space));
}

inline static void 
copy_position(float des[3], float src[3]) {
	des[0] = src[0];
	des[1] = src[1];
	des[2] = src[2];
}

static bool
change_mode(struct object * obj, bool set_watcher, bool set_marker) {
	bool change = false;
	if (obj->mode == 0) {
		if (set_watcher) {
			obj->mode = MODE_WATCHER;
		}
		if (set_marker) {
			obj->mode |= MODE_MARKER;
		}
		// set_watcher 和 set_marker 肯定至少一个为真的
		return true;
	}
	if (set_watcher) {
		if (!(obj->mode & MODE_WATCHER)) {
			obj->mode |= MODE_WATCHER;
			change = true;
		}
	} else {
		if (obj->mode & MODE_WATCHER) {
			obj->mode &= ~MODE_WATCHER;
			change = true;
		}
	}
	if (set_marker) {
		if (!(obj->mode & MODE_MARKER)) {
			obj->mode |= MODE_MARKER;
			change = true;
		}
	} else {
		if (obj->mode & MODE_MARKER) {
			obj->mode &= ~MODE_MARKER;
			change = true;
		}
	}
	return change;
}

// 两点间的距离是否小于 AOI 半径的一半
inline static bool
is_near(float p1[3], float p2[3]) {
	return DIST2(p1,p2) < AOI_RADIS2 * 0.25f ;
}

inline static float
dist2(struct object *p1, struct object *p2) {
	float d = DIST2(p1->position,p2->position);
	return d;
}

void
aoi_update(struct aoi_space * space , uint32_t id, const char * modestring , float pos[3]) {
	struct object * obj = map_query(space, space->object,id);
	int i;
	bool set_watcher = false;
	bool set_marker = false;

	for (i=0;modestring[i];++i) {
		char m = modestring[i];
		switch(m) {
		case 'w':
			set_watcher = true;
			break;
		case 'm':
			set_marker = true;
			break;
		case 'd':
			if (!(obj->mode & MODE_DROP)) {
				obj->mode = MODE_DROP;
				drop_object(space, obj);
			}
			return;
		}
	}

	if (obj->mode & MODE_DROP) {
		obj->mode &= ~MODE_DROP;
		// 要 drop 了，反而还在增加引用计数，如何理解？？？？
		grab_object(obj);
	}

	bool changed = change_mode(obj, set_watcher, set_marker);

	copy_position(obj->position, pos);
	
	// 与上一版本的座标比，走远了就要走两两比较的逻辑，不能再走热点对比较的逻辑
	// 不然 n 次小步阀移动之后可能两个对象互见了却因为彼此不是热点对，丟了 AOI 事件。
	if (changed || !is_near(pos, obj->last)) {
		// new object or change object mode
		// or position changed
		// 少数高速运动或跳转的对象会被打上 move 标记
		copy_position(obj->last , pos); // 关键点坐标
		obj->mode |= MODE_MOVE;
		++obj->version;
	} 
}

static void
drop_pair(struct aoi_space * space, struct pair_list *p) {
	drop_object(space, p->watcher);
	drop_object(space, p->marker);
	space->alloc(space->alloc_ud, p, sizeof(*p));
}

// 产生热点对的 aoi 消息
static void
flush_pair(struct aoi_space * space, aoi_Callback cb, void *ud) {
	struct pair_list **last = &(space->hot);
	struct pair_list *p = *last;
	while (p) {
		struct pair_list *next = p->next;
		if (p->watcher->version != p->watcher_version ||
			p->marker->version != p->marker_version ||
			(p->watcher->mode & MODE_DROP) ||
			(p->marker->mode & MODE_DROP)
			) {

			// blog: 只要有一个对象对象发生了改变，就将这个热点对抛弃。（因为一定有新的正确关联这两个对象的热点对在这个列表中）
			// 说人话就是： 如果实体对象的 version 变了，说明 obj->mode 也被设了 MODE_MOVE。后面会走 两两比较的逻辑。
			drop_pair(space, p);
			*last = next;

		// 没有位移或是移动距离小于 AOI 半径的一半
		} else {
			// 要结合这个 热点对是如何产生的来理解； 能进热点对说明上一个 tick 有大步长位移，当前 tick 却是小步长位移
			float distance2 = dist2(p->watcher , p->marker);
			// 两点间距离大于 AOI 半径 * 2
			if (distance2 > AOI_RADIS2 * 4) {
				drop_pair(space,p);
				*last = next;
			// 互相可见了
			} else if (distance2 < AOI_RADIS2) {
				// blog:当距离小于 AOI 半径时，发送 AOI 消息，并把自己从列表中删除
				cb(ud, p->watcher->id, p->marker->id);

				drop_pair(space,p);
				*last = next;
			} else { // 大于 AOI 的
				// blog:保留在列表中等待下个 tick 处理 
				last = &(p->next);
			}
		}
		p=next;
	}
}

static void
set_push_back(struct aoi_space * space, struct object_set * set, struct object *obj) {
	if (set->number >= set->cap) {
		int cap = set->cap * 2;
		void * tmp =  set->slot;
		// allocate
		set->slot = space->alloc(space->alloc_ud, NULL, cap * sizeof(struct object *));
		memcpy(set->slot, tmp ,  set->cap * sizeof(struct object *));
		// free
		space->alloc(space->alloc_ud, tmp, set->cap * sizeof(struct object *));
		set->cap = cap;
	}
	set->slot[set->number] = obj;
	++set->number;
}

// 根据状态位把 实体对象 加到相应的链表，并把 MODE_MOVE 标志去掉
static void
set_push(void * s, struct object * obj) {
	struct aoi_space * space = s;
	int mode = obj->mode;
	// 一个实体对象最少进 1 个链表，最多进 2 个链表
	if (mode & MODE_WATCHER) {
		if (mode & MODE_MOVE) {
			set_push_back(space, space->watcher_move , obj);
			obj->mode &= ~MODE_MOVE;
		} else {
			set_push_back(space, space->watcher_static , obj);
		}
	} 
	if (mode & MODE_MARKER) {
		if (mode & MODE_MOVE) {
			set_push_back(space, space->marker_move , obj);
			obj->mode &= ~MODE_MOVE;
		} else {
			set_push_back(space, space->marker_static , obj);
		}
	}
}

// 产生 aoi 消息或加入到热点对
static void
gen_pair(struct aoi_space * space, struct object * watcher, struct object * marker, aoi_Callback cb, void *ud) {
	if (watcher == marker) {
		return;
	}
	float distance2 = dist2(watcher, marker);
	// AOI 重叠了。即是视野范围内
	if (distance2 < AOI_RADIS2) {
		cb(ud, watcher->id, marker->id);
		return;
	}
	// 若距离比较远（ 两点间距离大于 AOI*2），他们将不会进入热点对
	if (distance2 > AOI_RADIS2 * 4) {
		return;
	}
	// 对于 AOI 半径 <= 距离 <= AOI 半径 * 2 的生成新的热点对
	struct pair_list * p = space->alloc(space->alloc_ud, NULL, sizeof(*p));
	p->watcher = watcher;
	grab_object(watcher);
	p->marker = marker;
	grab_object(marker);
	p->watcher_version = watcher->version;
	p->marker_version = marker->version;
	p->next = space->hot;
	space->hot = p;
}

// 产生消息，在过程中顺便生成热点对
static void
gen_pair_list(struct aoi_space *space, struct object_set * watcher, struct object_set * marker, aoi_Callback cb, void *ud) {
	int i,j;
	for (i=0;i<watcher->number;i++) {
		for (j=0;j<marker->number;j++) {
			gen_pair(space, watcher->slot[i], marker->slot[j],cb,ud);
		}
	}
}

void 
aoi_message(struct aoi_space *space, aoi_Callback cb, void *ud) {
	flush_pair(space,  cb, ud);
	// 各个 number 置 0，则相当于删除 4 个链表的全部元素（没有真删，要用时覆盖上去） 
	space->watcher_static->number = 0;
	space->watcher_move->number = 0;
	space->marker_static->number = 0;
	space->marker_move->number = 0;
	
	// 遍历全部实体对象一次，根据状态，重新生成 4 个链表
	// 一个实体对象最少进 1 个链表，最多进 2 个链表
	map_foreach(space->object, set_push , space);	

	// 两两比较 （至少有一个 move 参与比较）（两个 static 不互相比）
	gen_pair_list(space, space->watcher_static, space->marker_move, cb, ud);
	gen_pair_list(space, space->watcher_move, space->marker_static, cb, ud);
	gen_pair_list(space, space->watcher_move, space->marker_move, cb, ud);
}

static void *
default_alloc(void * ud, void *ptr, size_t sz) {
	if (ptr == NULL) {
		void *p = malloc(sz);
		return p;
	}
	free(ptr);
	return NULL;
}

struct aoi_space * 
aoi_new() {
	return aoi_create(default_alloc, NULL);
}
